# =============================================================================
#  Inbound mail for scribatic.com.
#
#  info@scribatic.com is received by SES, stored in a private bucket, and
#  forwarded to a personal inbox by a small Lambda. Mail stays inside this AWS
#  account: no third-party forwarding service holds the mapping, and the
#  destination address is never published in DNS — which the common free
#  forwarding tiers do require, and which would defeat the point of having a
#  domain address at all.
# =============================================================================

variable "mail_forward_to" {
  description = "Personal inbox that hello@ is forwarded to. Must be SES-verified while the account is in the sandbox."
  type        = string
  default     = "niroshpg@gmail.com"
}

variable "mail_local_part" {
  description = "Left-hand side of the public contact address."
  type        = string
  default     = "info"
}

locals {
  mail_address = "${var.mail_local_part}@${var.domain_name}"
  mail_prefix  = "inbound/"
}

# -----------------------------------------------------------------------------
#  Domain identity + DKIM
# -----------------------------------------------------------------------------

resource "aws_ses_domain_identity" "site" {
  domain = var.domain_name
}

resource "aws_ses_domain_dkim" "site" {
  domain = aws_ses_domain_identity.site.domain
}

resource "aws_route53_record" "dkim" {
  count = 3

  zone_id = data.aws_route53_zone.primary.zone_id
  name    = "${element(aws_ses_domain_dkim.site.dkim_tokens, count.index)}._domainkey"
  type    = "CNAME"
  ttl     = 600
  records = ["${element(aws_ses_domain_dkim.site.dkim_tokens, count.index)}.dkim.amazonses.com"]
}

# The destination must be a verified identity for as long as the account is in
# the SES sandbox. Creating this sends a confirmation mail that has to be
# clicked once; nothing forwards until it is.
resource "aws_ses_email_identity" "forward_to" {
  email = var.mail_forward_to
}

# -----------------------------------------------------------------------------
#  DNS: MX so the domain can receive, SPF and DMARC so what we send is trusted
# -----------------------------------------------------------------------------

resource "aws_route53_record" "mx" {
  zone_id = data.aws_route53_zone.primary.zone_id
  name    = var.domain_name
  type    = "MX"
  ttl     = 600
  records = ["10 inbound-smtp.${var.region}.amazonaws.com"]
}

resource "aws_route53_record" "spf" {
  zone_id = data.aws_route53_zone.primary.zone_id
  name    = var.domain_name
  type    = "TXT"
  ttl     = 600
  records = ["v=spf1 include:amazonses.com ~all"]
}

# p=none to start: this reports on alignment without asking anyone to reject
# mail, which is the right setting until the DKIM and SPF records have been
# observed passing in the wild.
resource "aws_route53_record" "dmarc" {
  zone_id = data.aws_route53_zone.primary.zone_id
  name    = "_dmarc.${var.domain_name}"
  type    = "TXT"
  ttl     = 600
  records = ["v=DMARC1; p=none; rua=mailto:${local.mail_address}"]
}

# -----------------------------------------------------------------------------
#  Storage for raw inbound mail
# -----------------------------------------------------------------------------

resource "aws_s3_bucket" "mail" {
  bucket = "scribatic-com-mail-${data.aws_caller_identity.current.account_id}"
  tags   = var.tags
}

resource "aws_s3_bucket_public_access_block" "mail" {
  bucket = aws_s3_bucket.mail.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "mail" {
  bucket = aws_s3_bucket.mail.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

# The bucket is a relay buffer, not an archive. Once a message has been
# forwarded the copy here is redundant, and keeping strangers' mail around
# indefinitely is a liability rather than an asset.
resource "aws_s3_bucket_lifecycle_configuration" "mail" {
  bucket = aws_s3_bucket.mail.id

  rule {
    id     = "expire-forwarded-mail"
    status = "Enabled"

    filter {
      prefix = local.mail_prefix
    }

    expiration {
      days = 30
    }
  }
}

data "aws_iam_policy_document" "mail_bucket" {
  statement {
    sid       = "AllowSESPut"
    actions   = ["s3:PutObject"]
    resources = ["${aws_s3_bucket.mail.arn}/${local.mail_prefix}*"]

    principals {
      type        = "Service"
      identifiers = ["ses.amazonaws.com"]
    }

    condition {
      test     = "StringEquals"
      variable = "AWS:SourceAccount"
      values   = [data.aws_caller_identity.current.account_id]
    }
  }
}

resource "aws_s3_bucket_policy" "mail" {
  bucket = aws_s3_bucket.mail.id
  policy = data.aws_iam_policy_document.mail_bucket.json

  depends_on = [aws_s3_bucket_public_access_block.mail]
}

# -----------------------------------------------------------------------------
#  Forwarder Lambda
# -----------------------------------------------------------------------------

data "archive_file" "forwarder" {
  type        = "zip"
  source_file = "${path.module}/../lambda/forwarder/index.py"
  output_path = "${path.module}/.build/forwarder.zip"
}

data "aws_iam_policy_document" "forwarder_assume" {
  statement {
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["lambda.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "forwarder" {
  name               = "scribatic-mail-forwarder"
  assume_role_policy = data.aws_iam_policy_document.forwarder_assume.json
  tags               = var.tags
}

data "aws_iam_policy_document" "forwarder" {
  statement {
    sid       = "ReadInboundMail"
    actions   = ["s3:GetObject"]
    resources = ["${aws_s3_bucket.mail.arn}/${local.mail_prefix}*"]
  }

  statement {
    sid       = "SendForwardedCopy"
    actions   = ["ses:SendRawEmail"]
    resources = ["*"]
  }

  statement {
    sid       = "Logs"
    actions   = ["logs:CreateLogGroup", "logs:CreateLogStream", "logs:PutLogEvents"]
    resources = ["arn:aws:logs:*:*:*"]
  }
}

resource "aws_iam_role_policy" "forwarder" {
  role   = aws_iam_role.forwarder.id
  policy = data.aws_iam_policy_document.forwarder.json
}

resource "aws_lambda_function" "forwarder" {
  function_name    = "scribatic-mail-forwarder"
  role             = aws_iam_role.forwarder.arn
  handler          = "index.handler"
  runtime          = "python3.12"
  timeout          = 30
  filename         = data.archive_file.forwarder.output_path
  source_code_hash = data.archive_file.forwarder.output_base64sha256
  tags             = var.tags

  environment {
    variables = {
      MAIL_BUCKET = aws_s3_bucket.mail.id
      MAIL_PREFIX = local.mail_prefix
      MAIL_FROM   = local.mail_address
      FORWARD_TO  = var.mail_forward_to
    }
  }
}

resource "aws_cloudwatch_log_group" "forwarder" {
  name              = "/aws/lambda/${aws_lambda_function.forwarder.function_name}"
  retention_in_days = 30
  tags              = var.tags
}

resource "aws_lambda_permission" "ses" {
  statement_id   = "AllowSESInvoke"
  action         = "lambda:InvokeFunction"
  function_name  = aws_lambda_function.forwarder.function_name
  principal      = "ses.amazonaws.com"
  source_account = data.aws_caller_identity.current.account_id
}

# -----------------------------------------------------------------------------
#  Receipt rules
# -----------------------------------------------------------------------------

resource "aws_ses_receipt_rule_set" "main" {
  rule_set_name = "scribatic-inbound"
}

resource "aws_ses_active_receipt_rule_set" "main" {
  rule_set_name = aws_ses_receipt_rule_set.main.rule_set_name
}

resource "aws_ses_receipt_rule" "forward" {
  name          = "forward-info"
  rule_set_name = aws_ses_receipt_rule_set.main.rule_set_name
  recipients    = [local.mail_address]
  enabled       = true
  scan_enabled  = true
  tls_policy    = "Require"

  # Order matters: the message has to be in S3 before the function that reads
  # it from S3 is invoked.
  s3_action {
    position          = 1
    bucket_name       = aws_s3_bucket.mail.id
    object_key_prefix = local.mail_prefix
  }

  lambda_action {
    position        = 2
    function_arn    = aws_lambda_function.forwarder.arn
    invocation_type = "Event"
  }

  depends_on = [
    aws_s3_bucket_policy.mail,
    aws_lambda_permission.ses,
  ]
}

output "contact_address" {
  description = "Public contact address; forwards to the configured inbox."
  value       = local.mail_address
}
