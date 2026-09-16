# =============================================================================
#  DMARC aggregate reports.
#
#  Reports go to their own address rather than to the public contact address.
#  Two reasons: a daily report from every large receiver buries actual mail
#  from actual people, and a dedicated recipient lets SES route reports to a
#  function that reads them instead of a function that forwards them.
#
#  The function is the point. A DMARC report is a zipped XML attachment, and
#  an attachment nobody opens is indistinguishable from no monitoring at all —
#  the domain could be spoofed for a month with the evidence sitting unread.
#  So the report is parsed in flight and re-sent as a message whose subject
#  line already says whether anything failed.
# =============================================================================

locals {
  dmarc_address = "dmarc@${var.domain_name}"

  # Raw mail and the parsed JSON share the mail bucket under their own
  # prefixes. Referenced from email.tf, which owns the bucket policy and the
  # lifecycle rules that cover all three prefixes.
  dmarc_prefix        = "dmarc/"
  dmarc_parsed_prefix = "dmarc-parsed/"
}

# -----------------------------------------------------------------------------
#  Reporter Lambda
# -----------------------------------------------------------------------------

data "archive_file" "dmarc" {
  type        = "zip"
  source_file = "${path.module}/../lambda/dmarc/index.py"
  output_path = "${path.module}/.build/dmarc.zip"
}

resource "aws_iam_role" "dmarc" {
  name               = "scribatic-dmarc-reporter"
  assume_role_policy = data.aws_iam_policy_document.forwarder_assume.json
  tags               = var.tags
}

data "aws_iam_policy_document" "dmarc" {
  statement {
    sid       = "ReadInboundReports"
    actions   = ["s3:GetObject"]
    resources = ["${aws_s3_bucket.mail.arn}/${local.dmarc_prefix}*"]
  }

  # Parsed reports are written, never read back by this function. Deciding
  # whether the domain has been clean long enough to move off p=none is a
  # question for later, asked against this history with the CLI.
  statement {
    sid       = "ArchiveParsedReports"
    actions   = ["s3:PutObject"]
    resources = ["${aws_s3_bucket.mail.arn}/${local.dmarc_parsed_prefix}*"]
  }

  statement {
    sid       = "SendDigest"
    actions   = ["ses:SendRawEmail"]
    resources = ["*"]
  }

  statement {
    sid       = "Logs"
    actions   = ["logs:CreateLogGroup", "logs:CreateLogStream", "logs:PutLogEvents"]
    resources = ["arn:aws:logs:*:*:*"]
  }
}

resource "aws_iam_role_policy" "dmarc" {
  role   = aws_iam_role.dmarc.id
  policy = data.aws_iam_policy_document.dmarc.json
}

resource "aws_lambda_function" "dmarc" {
  function_name = "scribatic-dmarc-reporter"
  role          = aws_iam_role.dmarc.arn
  handler       = "index.handler"
  runtime       = "python3.12"
  tags          = var.tags

  # Reverse-DNS lookups on unfamiliar source IPs are what turn a row of the
  # report into a name you can recognise. They are individually capped at two
  # seconds, but a report listing many distinct spoofing sources is exactly
  # the report that must not time out half-rendered.
  timeout     = 120
  memory_size = 256

  filename         = data.archive_file.dmarc.output_path
  source_code_hash = data.archive_file.dmarc.output_base64sha256

  environment {
    variables = {
      MAIL_BUCKET   = aws_s3_bucket.mail.id
      DMARC_PREFIX  = local.dmarc_prefix
      PARSED_PREFIX = local.dmarc_parsed_prefix
      MAIL_FROM     = local.dmarc_address
      REPORT_TO     = var.mail_forward_to
    }
  }
}

# The function mails itself an alert before re-raising on a report it cannot
# parse. Without this, the default two async retries would send that alert
# three times for one bad report.
resource "aws_lambda_function_event_invoke_config" "dmarc" {
  function_name          = aws_lambda_function.dmarc.function_name
  maximum_retry_attempts = 0
}

resource "aws_cloudwatch_log_group" "dmarc" {
  name              = "/aws/lambda/${aws_lambda_function.dmarc.function_name}"
  retention_in_days = 30
  tags              = var.tags
}

resource "aws_lambda_permission" "dmarc_ses" {
  statement_id   = "AllowSESInvoke"
  action         = "lambda:InvokeFunction"
  function_name  = aws_lambda_function.dmarc.function_name
  principal      = "ses.amazonaws.com"
  source_account = data.aws_caller_identity.current.account_id
}

# -----------------------------------------------------------------------------
#  Receipt rule
# -----------------------------------------------------------------------------

resource "aws_ses_receipt_rule" "dmarc" {
  name          = "parse-dmarc"
  rule_set_name = aws_ses_receipt_rule_set.main.rule_set_name
  recipients    = [local.dmarc_address]
  enabled       = true
  scan_enabled  = true
  tls_policy    = "Require"

  # Ordering is cosmetic — this rule and forward-info match disjoint
  # recipients — but a rule set with an undefined order is a rule set that
  # will surprise somebody the day a catch-all is added.
  after = aws_ses_receipt_rule.forward.name

  s3_action {
    position          = 1
    bucket_name       = aws_s3_bucket.mail.id
    object_key_prefix = local.dmarc_prefix
  }

  lambda_action {
    position        = 2
    function_arn    = aws_lambda_function.dmarc.arn
    invocation_type = "Event"
  }

  depends_on = [
    aws_s3_bucket_policy.mail,
    aws_lambda_permission.dmarc_ses,
  ]
}

# -----------------------------------------------------------------------------
#  The pipeline failing has to be louder than the pipeline saying "fine"
# -----------------------------------------------------------------------------

# A broken reporter produces silence, and silence is what a run of clean days
# looks like too. This is the one alarm that distinguishes them.
resource "aws_sns_topic" "ops" {
  name = "scribatic-ops-alerts"
  tags = var.tags
}

# Creating this sends a confirmation mail that has to be clicked once, the
# same as the SES destination identity. Nothing alarms until it is.
resource "aws_sns_topic_subscription" "ops_email" {
  topic_arn = aws_sns_topic.ops.arn
  protocol  = "email"
  endpoint  = var.mail_forward_to
}

resource "aws_cloudwatch_metric_alarm" "dmarc_errors" {
  alarm_name        = "scribatic-dmarc-reporter-errors"
  alarm_description = "The DMARC reporter threw. Reports are arriving and not being read."
  tags              = var.tags

  namespace   = "AWS/Lambda"
  metric_name = "Errors"
  dimensions  = { FunctionName = aws_lambda_function.dmarc.function_name }

  statistic           = "Sum"
  period              = 300
  evaluation_periods  = 1
  threshold           = 0
  comparison_operator = "GreaterThanThreshold"
  treat_missing_data  = "notBreaching"

  alarm_actions = [aws_sns_topic.ops.arn]
}

output "dmarc_report_address" {
  description = "rua= destination. Reports here are parsed, not forwarded."
  value       = local.dmarc_address
}
