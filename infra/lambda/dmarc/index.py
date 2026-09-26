"""
Make DMARC aggregate reports legible from the inbox.

Every large receiver sends one XML report per day summarising the mail that
claimed to be from scribatic.com. It arrives zipped or gzipped, as an
attachment. An attachment nobody opens is the same as no monitoring at all:
the failure mode of DMARC is a quiet one, where the domain is spoofed for
weeks and the evidence sits unread in a mailbox.

So this function does the opening. SES drops the raw mail in S3 and invokes
it; it unwraps the archive, evaluates every row, and sends back a message
whose *subject line* already carries the verdict. A clean day can be deleted
without being opened, and a bad one cannot be mistaken for a clean one.

The parsed form is also written to S3 as JSON, so the question the policy
actually turns on — "have we had a clean run long enough to move off p=none?"
— can be answered from history rather than from memory.
"""

import concurrent.futures
import dataclasses
import datetime
import gzip
import html
import io
import json
import os
import re
import socket
import traceback
import xml.etree.ElementTree as ElementTree
import zipfile
from email import message_from_bytes, policy
from email.message import EmailMessage

import boto3

s3 = boto3.client("s3")
ses = boto3.client("ses")

BUCKET = os.environ["MAIL_BUCKET"]
DMARC_PREFIX = os.environ.get("DMARC_PREFIX", "dmarc/")
PARSED_PREFIX = os.environ.get("PARSED_PREFIX", "dmarc-parsed/")
MAIL_FROM = os.environ["MAIL_FROM"]
REPORT_TO = os.environ["REPORT_TO"]

UNSAFE_IN_KEY = re.compile(r"[^A-Za-z0-9._-]")


# -----------------------------------------------------------------------------
#  Unwrapping
# -----------------------------------------------------------------------------

def _xml_documents(message):
    """Every DMARC XML document carried by one mail, decompressed."""
    documents = []
    for part in message.walk():
        if part.get_content_maintype() == "multipart":
            continue
        payload = part.get_payload(decode=True)
        if payload:
            documents.extend(_unwrap(part.get_filename() or "", payload))
    return documents


def _unwrap(filename, payload):
    """Zip, gzip or bare XML — receivers disagree, so sniff rather than trust
    the filename or the declared content type, both of which are frequently
    wrong (application/octet-stream for a zip is common)."""
    if payload[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            return [
                (name, archive.read(name))
                for name in archive.namelist()
                if name.lower().endswith(".xml")
            ]
    if payload[:2] == b"\x1f\x8b":
        return [(filename.removesuffix(".gz") or "report.xml", gzip.decompress(payload))]
    if payload.lstrip()[:5] == b"<?xml" or b"<feedback" in payload[:512]:
        return [(filename or "report.xml", payload)]
    return []


# -----------------------------------------------------------------------------
#  The report
# -----------------------------------------------------------------------------

@dataclasses.dataclass
class Row:
    source_ip: str
    count: int
    disposition: str
    dkim: str           # alignment, not whether the signature verified
    spf: str            # likewise
    header_from: str
    dkim_auth: list
    spf_auth: list
    reasons: list

    @property
    def passed(self):
        # DMARC passes on either aligned identifier. SPF alignment failing on
        # its own is expected here while SES owns the envelope sender.
        return self.dkim == "pass" or self.spf == "pass"


@dataclasses.dataclass
class Report:
    org: str
    report_id: str
    begin: int
    end: int
    domain: str
    policy: dict
    rows: list
    names: dict = dataclasses.field(default_factory=dict)

    @property
    def total(self):
        return sum(row.count for row in self.rows)

    @property
    def failed(self):
        return sum(row.count for row in self.rows if not row.passed)

    @property
    def failing_rows(self):
        return [row for row in self.rows if not row.passed]

    @property
    def enforced(self):
        """Messages the receiver actually quarantined or rejected."""
        return sum(row.count for row in self.rows if row.disposition not in ("", "none"))

    @property
    def clean(self):
        return self.failed == 0


def _strip_namespaces(root):
    for element in root.iter():
        if isinstance(element.tag, str) and "}" in element.tag:
            element.tag = element.tag.rpartition("}")[2]
    return root


def _text(element, path, default=""):
    if element is None:
        return default
    found = element.findtext(path)
    return default if found is None else found.strip()


def _parse(xml_bytes):
    root = _strip_namespaces(ElementTree.fromstring(xml_bytes))
    metadata = root.find("report_metadata")
    published = root.find("policy_published")

    rows = []
    for record in root.findall("record"):
        row = record.find("row")
        evaluated = row.find("policy_evaluated") if row is not None else None
        auth = record.find("auth_results")

        rows.append(Row(
            source_ip=_text(row, "source_ip"),
            count=int(_text(row, "count", "0") or 0),
            disposition=_text(evaluated, "disposition"),
            dkim=_text(evaluated, "dkim"),
            spf=_text(evaluated, "spf"),
            header_from=_text(record.find("identifiers"), "header_from"),
            dkim_auth=[
                {
                    "domain": _text(entry, "domain"),
                    "selector": _text(entry, "selector"),
                    "result": _text(entry, "result"),
                }
                for entry in (auth.findall("dkim") if auth is not None else [])
            ],
            spf_auth=[
                {"domain": _text(entry, "domain"), "result": _text(entry, "result")}
                for entry in (auth.findall("spf") if auth is not None else [])
            ],
            reasons=[
                f"{_text(entry, 'type')}: {_text(entry, 'comment')}".strip(": ")
                for entry in (evaluated.findall("reason") if evaluated is not None else [])
            ],
        ))

    date_range = metadata.find("date_range") if metadata is not None else None
    return Report(
        org=_text(metadata, "org_name", "unknown"),
        report_id=_text(metadata, "report_id", "unknown"),
        begin=int(_text(date_range, "begin", "0") or 0),
        end=int(_text(date_range, "end", "0") or 0),
        domain=_text(published, "domain", "unknown"),
        policy={
            key: _text(published, key)
            for key in ("p", "sp", "pct", "adkim", "aspf")
        },
        rows=rows,
    )


# -----------------------------------------------------------------------------
#  Presentation
# -----------------------------------------------------------------------------

def _resolve(ips, deadline=20.0):
    """Reverse-resolve every source IP at once, under one wall clock.

    Names are a convenience — they turn a bare IP into something recognisable
    as "ours" or "not ours" at a glance — so they must never be the reason a
    report goes unsent. Bounding them is awkward: gethostbyaddr takes no
    timeout because it is a C resolver call, and a global socket default
    would not reach it but *would* reach into botocore's connections. Hence a
    deadline over a thread pool. Whatever has not answered by then is blank.
    """
    names = {}
    if not ips:
        return names

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=min(16, len(ips)))
    try:
        pending = {pool.submit(socket.gethostbyaddr, ip): ip for ip in ips}
        done, _ = concurrent.futures.wait(pending, timeout=deadline)
        for future in done:
            try:
                names[pending[future]] = future.result()[0]
            except OSError:
                pass
    finally:
        pool.shutdown(wait=False, cancel_futures=True)
    return names


def _day(timestamp):
    return datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime("%Y-%m-%d")


def _window(report):
    first, last = _day(report.begin), _day(report.end)
    return first if first == last else f"{first} → {last}"


def _subject(report):
    # The subject is the whole point: it has to answer "do I need to look at
    # this?" on its own, and it has to be filterable. Gmail rules keyed on
    # "[DMARC ok]" and "[DMARC FAIL]" then do the triage.
    when = _window(report)
    if report.clean:
        return f"[DMARC ok] {report.org} — {report.total} msgs, all aligned — {when}"
    ips = len({row.source_ip for row in report.failing_rows})
    return (
        f"[DMARC FAIL] {report.org} — {report.failed} of {report.total} unaligned "
        f"from {ips} IP{'s' if ips != 1 else ''} — {when}"
    )


def _verdict_line(report):
    if report.clean:
        return f"ok — all {report.total} message(s) passed DMARC"
    line = f"FAIL — {report.failed} of {report.total} message(s) failed DMARC"
    if report.enforced:
        line += f"; {report.enforced} were quarantined or rejected by the receiver"
    return line


def _render_text(report):
    policy_text = " ".join(f"{key}={value}" for key, value in report.policy.items() if value)
    lines = [
        f"{report.domain} — DMARC aggregate report",
        "",
        f"  Reporter   {report.org}  (report {report.report_id})",
        f"  Window     {_window(report)} UTC",
        f"  Policy     {policy_text}",
        "",
        f"  VERDICT    {_verdict_line(report)}",
        "",
        f"  {'SOURCE':<24}{'MSGS':>6}  {'DKIM':<6}{'SPF':<6}{'DMARC':<7}{'ACTION'}",
        f"  {'-' * 62}",
    ]
    for row in sorted(report.rows, key=lambda r: (r.passed, -r.count)):
        lines.append(
            f"  {row.source_ip:<24}{row.count:>6}  {row.dkim or '-':<6}{row.spf or '-':<6}"
            f"{('pass' if row.passed else 'FAIL'):<7}{row.disposition or 'none'}"
        )
        host = report.names.get(row.source_ip)
        if host:
            lines.append(f"      {host}")
        for reason in row.reasons:
            lines.append(f"      reason: {reason}")
        if not row.passed:
            for entry in row.dkim_auth:
                lines.append(
                    f"      dkim  {entry['domain']} "
                    f"selector={entry['selector'] or '-'} {entry['result']}"
                )
            for entry in row.spf_auth:
                lines.append(f"      spf   {entry['domain']} {entry['result']}")
    if not report.clean:
        lines += [
            "",
            "  A source that is not SES is either a third-party sender nobody wrote",
            "  down, or somebody spoofing the domain. Check the hostnames above",
            "  before changing the policy.",
        ]
    return "\n".join(lines) + "\n"


def _render_html(report):
    escape = html.escape
    accent = "#1a7f37" if report.clean else "#b42318"
    tint = "#e8f5ec" if report.clean else "#fdeceb"

    body = []
    for row in sorted(report.rows, key=lambda r: (r.passed, -r.count)):
        host = report.names.get(row.source_ip)
        detail = []
        if host:
            detail.append(escape(host))
        detail += [escape(reason) for reason in row.reasons]
        if not row.passed:
            detail += [
                f"dkim {escape(entry['domain'])} &rarr; {escape(entry['result'])}"
                for entry in row.dkim_auth
            ]
            detail += [
                f"spf {escape(entry['domain'])} &rarr; {escape(entry['result'])}"
                for entry in row.spf_auth
            ]
        source = escape(row.source_ip)
        if detail:
            source += (
                '<div style="color:#667085;font-size:12px;padding-top:3px">'
                + "<br>".join(detail)
                + "</div>"
            )
        body.append(
            '<tr>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0;font-family:ui-monospace,Menlo,monospace">{source}</td>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0;text-align:right">{row.count}</td>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0">{escape(row.dkim or "-")}</td>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0">{escape(row.spf or "-")}</td>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0;font-weight:600;'
            f'color:{"#1a7f37" if row.passed else "#b42318"}">{"pass" if row.passed else "FAIL"}</td>'
            f'<td style="padding:8px 10px;border-top:1px solid #eaecf0">{escape(row.disposition or "none")}</td>'
            '</tr>'
        )

    policy_text = escape(" ".join(f"{k}={v}" for k, v in report.policy.items() if v))
    head = (
        '<th style="padding:6px 10px;text-align:left;font-size:12px;'
        'text-transform:uppercase;letter-spacing:.04em;color:#667085">'
    )
    return f"""\
<div style="font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
            font-size:14px;color:#101828;max-width:760px">
  <h2 style="margin:0 0 2px;font-size:17px">{escape(report.domain)} &mdash; DMARC aggregate report</h2>
  <div style="color:#667085;font-size:13px">
    {escape(report.org)} &middot; {escape(_window(report))} UTC &middot; {policy_text}
  </div>
  <div style="margin:14px 0;padding:10px 14px;border-radius:6px;
              background:{tint};border-left:4px solid {accent};color:{accent};font-weight:600">
    {escape(_verdict_line(report))}
  </div>
  <table cellspacing="0" cellpadding="0" style="width:100%;border-collapse:collapse;font-size:13px">
    <tr>{head}Source</th>{head}Msgs</th>{head}DKIM</th>{head}SPF</th>{head}DMARC</th>{head}Action</th></tr>
    {"".join(body)}
  </table>
  <p style="color:#667085;font-size:12px;margin-top:16px">
    DKIM and SPF above are <em>alignment</em>, not whether the check itself passed.
    DMARC passes on either one. SPF alignment fails by design while SES owns the
    envelope sender; a custom MAIL FROM domain would fix it.<br>
    Report {escape(report.report_id)} &middot; raw XML kept 30 days, parsed JSON 400.
  </p>
</div>
"""


def _send(subject, text, html_body=None):
    message = EmailMessage()
    message["Subject"] = subject
    message["From"] = MAIL_FROM
    message["To"] = REPORT_TO
    message.set_content(text)
    if html_body:
        message.add_alternative(html_body, subtype="html")
    ses.send_raw_email(
        Source=MAIL_FROM,
        Destinations=[REPORT_TO],
        RawMessage={"Data": message.as_bytes()},
    )


# -----------------------------------------------------------------------------
#  Entry point
# -----------------------------------------------------------------------------

def _archive(report, document_name):
    safe = lambda value: UNSAFE_IN_KEY.sub("_", value)[:120]
    key = f"{PARSED_PREFIX}{_day(report.begin)}/{safe(report.org)}!{safe(report.report_id)}.json"
    s3.put_object(
        Bucket=BUCKET,
        Key=key,
        Body=json.dumps(
            {
                "source_document": document_name,
                "org": report.org,
                "report_id": report.report_id,
                "begin": report.begin,
                "end": report.end,
                "domain": report.domain,
                "policy": report.policy,
                "total": report.total,
                "failed": report.failed,
                "enforced": report.enforced,
                "names": report.names,
                "rows": [dataclasses.asdict(row) for row in report.rows],
            },
            indent=2,
        ).encode(),
        ContentType="application/json",
    )
    return key


def handler(event, context):
    sent = 0
    for record in event.get("Records", []):
        message_id = record["ses"]["mail"]["messageId"]
        key = f"{DMARC_PREFIX}{message_id}"
        try:
            raw = s3.get_object(Bucket=BUCKET, Key=key)["Body"].read()
            documents = _xml_documents(message_from_bytes(raw, policy=policy.default))
            if not documents:
                raise ValueError("no DMARC XML found in the message")

            for name, xml_bytes in documents:
                report = _parse(xml_bytes)
                report.names = _resolve({row.source_ip for row in report.rows})
                archived = _archive(report, name)
                _send(_subject(report), _render_text(report), _render_html(report))
                sent += 1
                print(
                    f"{report.org} {report.report_id}: {report.total - report.failed}"
                    f"/{report.total} aligned, archived at {archived}"
                )
        except Exception:
            # Never fail silently: a parser that breaks on a format we have not
            # seen looks exactly like a run of clean days. Say so in the inbox,
            # then re-raise so the CloudWatch error alarm fires as well. Retries
            # are disabled on this function, so this happens once.
            detail = traceback.format_exc()
            _send(
                f"[DMARC unreadable] could not read a report — {message_id}",
                "A DMARC report arrived that this function could not parse.\n\n"
                f"  Bucket   {BUCKET}\n"
                f"  Key      {key}\n\n"
                "The raw mail is in S3 for 30 days. Read it by hand with:\n\n"
                f"  aws s3 cp s3://{BUCKET}/{key} - | less\n\n"
                f"{detail}",
            )
            raise

    return {"reports": sent}
