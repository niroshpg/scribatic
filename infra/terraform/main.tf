# =============================================================================
#  scribatic.com — static "coming soon" landing page.
#
#  Shape: a private S3 bucket holding the site, reachable ONLY through a
#  CloudFront distribution that authenticates to it with Origin Access Control.
#  The bucket has no public access and no website endpoint, so there is no way
#  to reach an object except through the distribution.
# =============================================================================

data "aws_caller_identity" "current" {}

data "aws_route53_zone" "primary" {
  name         = "${var.domain_name}."
  private_zone = false
}

locals {
  # Bucket names are globally unique across every AWS account, so the account
  # id keeps this deterministic rather than a coin flip on "scribatic-com-site".
  bucket_name = "scribatic-com-site-${data.aws_caller_identity.current.account_id}"

  aliases = [var.domain_name, "www.${var.domain_name}"]

  site_dir = "${path.module}/../site"

  content_types = {
    "html"        = "text/html; charset=utf-8"
    "css"         = "text/css; charset=utf-8"
    "js"          = "application/javascript; charset=utf-8"
    "svg"         = "image/svg+xml"
    "png"         = "image/png"
    "jpg"         = "image/jpeg"
    "ico"         = "image/x-icon"
    "txt"         = "text/plain; charset=utf-8"
    "webmanifest" = "application/manifest+json"
    "woff2"       = "font/woff2"
  }
}

# -----------------------------------------------------------------------------
#  Origin bucket
# -----------------------------------------------------------------------------

resource "aws_s3_bucket" "site" {
  bucket = local.bucket_name
  tags   = var.tags
}

# ACLs disabled entirely; the bucket policy below is the only grant.
resource "aws_s3_bucket_ownership_controls" "site" {
  bucket = aws_s3_bucket.site.id

  rule {
    object_ownership = "BucketOwnerEnforced"
  }
}

resource "aws_s3_bucket_public_access_block" "site" {
  bucket = aws_s3_bucket.site.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "site" {
  bucket = aws_s3_bucket.site.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_versioning" "site" {
  bucket = aws_s3_bucket.site.id

  versioning_configuration {
    status = "Enabled"
  }
}

# -----------------------------------------------------------------------------
#  Site objects
# -----------------------------------------------------------------------------

resource "aws_s3_object" "site" {
  for_each = fileset(local.site_dir, "**/*")

  bucket       = aws_s3_bucket.site.id
  key          = each.value
  source       = "${local.site_dir}/${each.value}"
  etag         = filemd5("${local.site_dir}/${each.value}")
  content_type = lookup(local.content_types, lower(try(element(split(".", each.value), length(split(".", each.value)) - 1), "")), "application/octet-stream")

  # Short TTL on a pre-launch page: the copy will change more often than the
  # infrastructure, and 5 minutes avoids needing an invalidation for each edit.
  cache_control = "public, max-age=300, must-revalidate"

  tags = var.tags
}

# -----------------------------------------------------------------------------
#  Certificate (us-east-1, DNS-validated through the hosted zone)
# -----------------------------------------------------------------------------

resource "aws_acm_certificate" "site" {
  provider = aws.us_east_1

  domain_name               = var.domain_name
  subject_alternative_names = ["www.${var.domain_name}"]
  validation_method         = "DNS"

  tags = var.tags

  lifecycle {
    create_before_destroy = true
  }
}

resource "aws_route53_record" "cert_validation" {
  for_each = {
    for option in aws_acm_certificate.site.domain_validation_options :
    option.domain_name => {
      name   = option.resource_record_name
      record = option.resource_record_value
      type   = option.resource_record_type
    }
  }

  zone_id         = data.aws_route53_zone.primary.zone_id
  name            = each.value.name
  type            = each.value.type
  records         = [each.value.record]
  ttl             = 60
  allow_overwrite = true
}

resource "aws_acm_certificate_validation" "site" {
  provider = aws.us_east_1

  certificate_arn         = aws_acm_certificate.site.arn
  validation_record_fqdns = [for record in aws_route53_record.cert_validation : record.fqdn]
}
