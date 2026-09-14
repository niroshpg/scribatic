"""
Forward mail received at scribatic.com on to a personal inbox.

SES stores the raw message in S3 and then invokes this function. The message is
re-sent from an address in the verified domain rather than relayed verbatim:
resending with the original From intact would fail SPF and DMARC at the
receiving end, because this account is not authorised to send as the stranger
who wrote to us. The original sender is preserved in Reply-To, so replying from
the inbox still reaches them.
"""

import email
import email.policy
import os

import boto3

s3 = boto3.client("s3")
ses = boto3.client("ses")

BUCKET = os.environ["MAIL_BUCKET"]
PREFIX = os.environ.get("MAIL_PREFIX", "")
MAIL_FROM = os.environ["MAIL_FROM"]
FORWARD_TO = os.environ["FORWARD_TO"]

# Headers that belong to the original transmission. Carrying them over makes
# the forwarded copy look like a forgery to the receiving MTA.
STRIP = {
    "return-path", "sender", "message-id", "dkim-signature",
    "received-spf", "authentication-results", "from", "reply-to",
    "to", "cc", "bcc",
}


def handler(event, context):
    for record in event.get("Records", []):
        message_id = record["ses"]["mail"]["messageId"]
        key = f"{PREFIX}{message_id}"

        raw = s3.get_object(Bucket=BUCKET, Key=key)["Body"].read()
        original = email.message_from_bytes(raw, policy=email.policy.default)

        original_from = original.get("From", "unknown sender")
        subject = original.get("Subject", "(no subject)")

        for header in list(original.keys()):
            if header.lower() in STRIP:
                del original[header]

        original["From"] = MAIL_FROM
        original["Reply-To"] = original_from
        original["To"] = FORWARD_TO
        original["X-Original-From"] = original_from

        ses.send_raw_email(
            Source=MAIL_FROM,
            Destinations=[FORWARD_TO],
            RawMessage={"Data": original.as_bytes()},
        )

        print(f"forwarded {message_id!r} from {original_from!r} subject {subject!r}")

    return {"disposition": "STOP_RULE_SET"}
