variable "aws_profile" {
  description = "Local AWS CLI profile used for this stack."
  type        = string
  default     = "personal"
}

variable "region" {
  description = "Region for the origin bucket. CloudFront and ACM are handled separately."
  type        = string
  default     = "ap-southeast-2"
}

variable "domain_name" {
  description = "Apex domain. Must already have a public hosted zone in this account."
  type        = string
  default     = "scribatic.com"
}

variable "tags" {
  description = "Applied to every taggable resource."
  type        = map(string)
  default = {
    Project   = "scribatic"
    Component = "landing"
    ManagedBy = "terraform"
  }
}
