output "bucket_name" {
  description = "Origin bucket holding the site."
  value       = aws_s3_bucket.site.id
}

output "distribution_id" {
  description = "Use with: aws cloudfront create-invalidation --distribution-id <id> --paths '/*'"
  value       = aws_cloudfront_distribution.site.id
}

output "distribution_domain_name" {
  description = "CloudFront domain, reachable before DNS propagates."
  value       = aws_cloudfront_distribution.site.domain_name
}

output "certificate_arn" {
  value = aws_acm_certificate_validation.site.certificate_arn
}

output "site_url" {
  value = "https://${var.domain_name}"
}
