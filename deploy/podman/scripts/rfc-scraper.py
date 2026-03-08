#!/usr/bin/env python3
"""RFC scraper for the iohttp HTTP server project.

Searches datatracker.ietf.org API for RFCs and active Internet-Drafts
relevant to HTTP/1.1, HTTP/2, HTTP/3, QUIC, TLS, WebSocket, and web security.
Outputs a structured Markdown registry.

Usage:
    python3 rfc-scraper.py                          # stdout
    python3 rfc-scraper.py -o iohttp-rfcs.md       # file
    python3 rfc-scraper.py --download docs/rfc/     # download .txt
    python3 rfc-scraper.py --json                   # JSON output
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import requests
from requests.adapters import HTTPAdapter, Retry

log = logging.getLogger("rfc-scraper")

# ── Datatracker API ───────────────────────────────────────────────────

DATATRACKER_BASE = "https://datatracker.ietf.org"
DATATRACKER_API = f"{DATATRACKER_BASE}/api/v1"
RFC_EDITOR_BASE = "https://www.rfc-editor.org"
USER_AGENT = "iohttp-rfc-scraper/1.0"
REQUEST_TIMEOUT = 15
RATE_LIMIT_DELAY = 0.3

# Full URI references for Tastypie filtered fields
DOCTYPE_RFC = f"{DATATRACKER_API}/name/doctypename/rfc/"
DOCTYPE_DRAFT = f"{DATATRACKER_API}/name/doctypename/draft/"


# ── Data model ────────────────────────────────────────────────────────


@dataclass
class RfcEntry:
    """Metadata for a single RFC."""

    number: int
    title: str
    status: str = ""
    pages: int | None = None
    categories: set[str] = field(default_factory=set)

    @property
    def url(self) -> str:
        return f"{RFC_EDITOR_BASE}/rfc/rfc{self.number}"

    @property
    def txt_url(self) -> str:
        return f"{RFC_EDITOR_BASE}/rfc/rfc{self.number}.txt"


@dataclass
class DraftEntry:
    """Metadata for an active Internet-Draft."""

    name: str
    title: str
    status: str = "ACTIVE DRAFT"
    rev: str = ""
    categories: set[str] = field(default_factory=set)

    @property
    def url(self) -> str:
        return f"{DATATRACKER_BASE}/doc/{self.name}/"


# ── Search categories ────────────────────────────────────────────────

CATEGORIES: dict[str, list[str]] = {
    "HTTP Core": [
        "HTTP semantics",
        "HTTP/1.1",
        "HTTP caching",
        "HTTP range requests",
        "HTTP conditional requests",
        "HTTP authentication framework",
        "HTTP content negotiation",
        "HTTP status code",
        "HTTP method",
    ],
    "HTTP/2": [
        "HTTP/2",
        "HPACK",
        "HTTP/2 server push",
        "HTTP priority",
        "HTTP extensible priorities",
    ],
    "HTTP/3 & QUIC": [
        "HTTP/3",
        "QPACK",
        "QUIC transport",
        "QUIC TLS",
        "QUIC recovery",
        "QUIC datagram",
        "QUIC version negotiation",
        "QUIC multipath",
    ],
    "TLS": [
        "TLS 1.3",
        "TLS 1.2",
        "TLS extensions",
        "TLS ALPN",
        "TLS SNI",
        "TLS certificate",
        "TLS session resumption",
        "TLS encrypted client hello",
        "TLS post-quantum",
        "OCSP stapling",
    ],
    "WebSocket": [
        "WebSocket",
        "WebSocket compression",
        "WebSocket HTTP/2",
        "WebSocket HTTP/3",
    ],
    "Security Headers": [
        "HTTP Strict Transport Security",
        "Content Security Policy",
        "CORS",
        "X-Frame-Options",
        "Certificate Transparency",
    ],
    "Authentication": [
        "HTTP authentication",
        "Bearer token",
        "OAuth 2.0",
        "JSON Web Token",
        "JSON Web Signature",
        "JSON Web Key",
        "HTTP cookies",
    ],
    "Compression": [
        "Brotli",
        "Zstandard",
        "DEFLATE",
        "GZIP",
        "header compression",
        "HPACK",
        "QPACK",
    ],
    "URI & Content": [
        "URI",
        "URI template",
        "MIME",
        "multipart form-data",
        "JSON",
        "Structured Fields",
        "Web Linking",
    ],
    "Server Features": [
        "Server-Sent Events",
        "Early Hints",
        "Problem Details HTTP",
        "PROXY protocol",
        "Rate limiting HTTP",
        "HTTP Client Hints",
    ],
    "Cryptography": [
        "AES-GCM",
        "ChaCha20-Poly1305",
        "Curve25519",
        "X25519",
        "HKDF",
        "AEAD",
        "post-quantum cryptography",
    ],
}

# Known critical RFCs — manually curated
KNOWN_CRITICAL: dict[int, tuple[str, str]] = {
    # HTTP Core
    9110: ("HTTP Semantics", "INTERNET STANDARD"),
    9111: ("HTTP Caching", "INTERNET STANDARD"),
    9112: ("HTTP/1.1", "INTERNET STANDARD"),
    9113: ("HTTP/2", "PROPOSED STANDARD"),
    9114: ("HTTP/3", "PROPOSED STANDARD"),
    9218: ("Extensible Prioritization Scheme for HTTP", "PROPOSED STANDARD"),
    9530: ("Digest Fields", "PROPOSED STANDARD"),
    9457: ("Problem Details for HTTP APIs", "INTERNET STANDARD"),
    8297: ("An HTTP Status Code for Indicating Hints", "EXPERIMENTAL"),
    7694: ("HTTP Client-Initiated Content-Encoding", "PROPOSED STANDARD"),
    # QUIC
    9000: ("QUIC Transport Protocol", "PROPOSED STANDARD"),
    9001: ("Using TLS to Secure QUIC", "PROPOSED STANDARD"),
    9002: ("QUIC Loss Detection and Congestion Control", "PROPOSED STANDARD"),
    9221: ("QUIC Unreliable Datagram Extension", "PROPOSED STANDARD"),
    9369: ("QUIC Version 2", "PROPOSED STANDARD"),
    9204: ("QPACK: Field Compression for HTTP/3", "PROPOSED STANDARD"),
    8999: ("Version-Independent Properties of QUIC", "PROPOSED STANDARD"),
    9368: ("Compatible Version Negotiation for QUIC", "PROPOSED STANDARD"),
    9287: ("Greasing the QUIC Bit", "PROPOSED STANDARD"),
    # TLS
    8446: ("TLS 1.3", "PROPOSED STANDARD"),
    5246: ("TLS 1.2", "PROPOSED STANDARD"),
    7301: ("TLS ALPN", "PROPOSED STANDARD"),
    6066: ("TLS Extensions (SNI etc.)", "PROPOSED STANDARD"),
    5077: ("TLS Session Resumption without Server-Side State", "PROPOSED STANDARD"),
    8879: ("TLS Certificate Compression", "PROPOSED STANDARD"),
    9325: ("Recommendations for TLS/DTLS 1.2 and Older", "BCP"),
    8701: ("Applying GREASE to TLS Extensibility", "PROPOSED STANDARD"),
    8996: ("Deprecating TLS 1.0 and TLS 1.1", "BCP"),
    7905: ("ChaCha20-Poly1305 Cipher Suites for TLS", "PROPOSED STANDARD"),
    9848: ("Deprecating Obsolete Key Exchange in TLS 1.2", "PROPOSED STANDARD"),
    9849: ("Deprecating Obsolete Cipher Suites in TLS", "PROPOSED STANDARD"),
    # WebSocket
    6455: ("WebSocket Protocol", "PROPOSED STANDARD"),
    8441: ("WebSocket over HTTP/2", "PROPOSED STANDARD"),
    9220: ("Bootstrapping WebSockets with HTTP/3", "PROPOSED STANDARD"),
    7692: ("Compression Extensions for WebSocket", "PROPOSED STANDARD"),
    # Authentication & Security
    7235: ("HTTP/1.1: Authentication", "PROPOSED STANDARD"),
    7617: ("The 'Basic' HTTP Authentication Scheme", "PROPOSED STANDARD"),
    6750: ("OAuth 2.0 Bearer Token Usage", "PROPOSED STANDARD"),
    6749: ("OAuth 2.0", "PROPOSED STANDARD"),
    7519: ("JSON Web Token (JWT)", "PROPOSED STANDARD"),
    7515: ("JSON Web Signature (JWS)", "PROPOSED STANDARD"),
    7516: ("JSON Web Encryption (JWE)", "PROPOSED STANDARD"),
    7517: ("JSON Web Key (JWK)", "PROPOSED STANDARD"),
    7636: ("OAuth PKCE", "PROPOSED STANDARD"),
    6797: ("HTTP Strict Transport Security (HSTS)", "PROPOSED STANDARD"),
    6265: ("HTTP State Management Mechanism (Cookies)", "PROPOSED STANDARD"),
    8705: ("OAuth 2.0 Mutual-TLS", "PROPOSED STANDARD"),
    9449: ("OAuth 2.0 DPoP", "PROPOSED STANDARD"),
    9421: ("HTTP Message Signatures", "PROPOSED STANDARD"),
    # Compression
    7932: ("Brotli Compressed Data Format", "INFORMATIONAL"),
    8878: ("Zstandard Compression", "INFORMATIONAL"),
    1951: ("DEFLATE Compressed Data Format", "INFORMATIONAL"),
    1952: ("GZIP File Format Specification", "INFORMATIONAL"),
    7541: ("HPACK: Header Compression for HTTP/2", "PROPOSED STANDARD"),
    # URI & Content
    3986: ("Uniform Resource Identifier (URI)", "INTERNET STANDARD"),
    6570: ("URI Template", "PROPOSED STANDARD"),
    8259: ("JSON", "INTERNET STANDARD"),
    7578: ("Returning Values from Forms: multipart/form-data", "PROPOSED STANDARD"),
    8288: ("Web Linking", "PROPOSED STANDARD"),
    8941: ("Structured Field Values for HTTP", "PROPOSED STANDARD"),
    # Cryptography
    5116: ("AEAD Interface", "PROPOSED STANDARD"),
    8439: ("ChaCha20 and Poly1305 for IETF", "INFORMATIONAL"),
    7748: ("Elliptic Curves for Security (X25519/X448)", "INFORMATIONAL"),
    5869: ("HKDF", "INFORMATIONAL"),
    5288: ("AES-GCM Cipher Suites for TLS", "PROPOSED STANDARD"),
    9180: ("Hybrid Public Key Encryption (HPKE)", "INFORMATIONAL"),
    # Certificates
    5280: ("X.509 PKI Certificate", "PROPOSED STANDARD"),
    6960: ("OCSP", "PROPOSED STANDARD"),
    6961: ("TLS Multiple Certificate Status Extension (OCSP Stapling)", "PROPOSED STANDARD"),
    6962: ("Certificate Transparency", "EXPERIMENTAL"),
    8555: ("ACME (Automatic Certificate Management)", "PROPOSED STANDARD"),
    # Encoding
    4648: ("Base Encodings (Base16, Base32, Base64)", "PROPOSED STANDARD"),
    2045: ("MIME Part One: Format of Internet Message Bodies", "DRAFT STANDARD"),
    # Security BCP
    7525: ("Recommendations for Secure Use of TLS and DTLS", "BCP"),
    # Transport
    9293: ("TCP Specification", "INTERNET STANDARD"),
    8305: ("Happy Eyeballs v2", "PROPOSED STANDARD"),
}

# Manually curated important drafts
IMPORTANT_DRAFTS: dict[str, str] = {
    "draft-ietf-tls-esni": "Encrypted Client Hello (ECH)",
    "draft-ietf-tls-hybrid-design": "Hybrid Key Exchange in TLS 1.3 (post-quantum)",
    "draft-ietf-httpbis-resumable-upload": "Resumable Uploads for HTTP",
    "draft-ietf-httpbis-sfbis": "Structured Field Values for HTTP (bis)",
    "draft-ietf-httpbis-retrofit": "Retrofit Structured Fields for HTTP",
    "draft-ietf-webtrans-http3": "WebTransport over HTTP/3",
    "draft-ietf-quic-multipath": "QUIC Multipath",
    "draft-ietf-quic-ack-frequency": "QUIC ACK Frequency",
    "draft-ietf-tls-ecdhe-mlkem": "Post-quantum hybrid ECDHE-MLKEM for TLS 1.3",
    "draft-ietf-tls-mlkem": "ML-KEM Post-Quantum Key Agreement for TLS 1.3",
    "draft-ietf-tls-rfc8446bis": "TLS 1.3 (maintenance update)",
    "draft-ietf-tls-svcb-ech": "Bootstrapping TLS ECH with DNS SVCB",
    "draft-ietf-ccwg-bbr": "BBR Congestion Control (BBRv3)",
    "draft-ietf-httpbis-compression-dictionary": "Compression Dictionary Transport for HTTP",
    "draft-ietf-httpbis-unprompted-auth": "Unprompted HTTP Authentication",
}

# Critical RFC groups for the summary section
CRITICAL_GROUPS: dict[str, list[int]] = {
    "HTTP Core": [
        9110,
        9111,
        9112,
        9113,
        9114,
        9218,
        9457,
        8297,
    ],
    "QUIC (HTTP/3 transport)": [
        9000,
        9001,
        9002,
        9221,
        9369,
        9204,
        8999,
    ],
    "TLS (security layer)": [
        8446,
        5246,
        7301,
        6066,
        5077,
        8879,
        9325,
        8701,
        8996,
        7905,
        9848,
        9849,
    ],
    "WebSocket": [
        6455,
        8441,
        9220,
        7692,
    ],
    "Authentication & Security": [
        7235,
        7617,
        6750,
        6749,
        7519,
        7515,
        7636,
        6797,
        6265,
        8705,
        9449,
        9421,
    ],
    "Compression": [
        7932,
        8878,
        1951,
        1952,
        7541,
        9204,
    ],
    "URI & Content": [
        3986,
        6570,
        8259,
        7578,
        8288,
        8941,
    ],
    "Cryptography": [
        5116,
        5288,
        8439,
        7748,
        5869,
        9180,
    ],
    "Certificates & PKI": [
        5280,
        6960,
        6961,
        6962,
        8555,
    ],
    "Security BCP & Operations": [
        7525,
        9325,
        8996,
    ],
}

# Protocol-to-RFC mapping for the matrix section
PROTOCOL_MATRIX: dict[str, list[str]] = {
    "HTTP/1.1 (baseline)": [
        "RFC 9110 (HTTP Semantics)",
        "RFC 9112 (HTTP/1.1)",
        "RFC 9111 (HTTP Caching)",
        "RFC 8446 (TLS 1.3)",
        "RFC 7301 (ALPN)",
        "RFC 6066 (SNI)",
    ],
    "HTTP/2": [
        "RFC 9113 (HTTP/2)",
        "RFC 7541 (HPACK)",
        "RFC 9218 (Extensible Priorities)",
        "RFC 8441 (WebSocket over HTTP/2)",
    ],
    "HTTP/3": [
        "RFC 9114 (HTTP/3)",
        "RFC 9204 (QPACK)",
        "RFC 9000 (QUIC Transport)",
        "RFC 9001 (QUIC + TLS)",
        "RFC 9002 (Loss Detection)",
        "RFC 9220 (WebSocket over HTTP/3)",
    ],
    "WebSocket": [
        "RFC 6455 (WebSocket Protocol)",
        "RFC 7692 (Compression Extensions)",
        "RFC 8441 (WebSocket over HTTP/2)",
        "RFC 9220 (WebSocket over HTTP/3)",
    ],
    "TLS (security layer)": [
        "RFC 8446 (TLS 1.3)",
        "RFC 7301 (ALPN)",
        "RFC 6066 (SNI)",
        "RFC 8879 (Certificate Compression)",
        "RFC 9325 (Secure Use of TLS)",
        "RFC 7905 (ChaCha20-Poly1305 for TLS)",
        "draft-ietf-tls-esni (ECH)",
    ],
    "Authentication": [
        "RFC 7235 (HTTP Authentication)",
        "RFC 7617 (Basic Auth)",
        "RFC 6750 (Bearer Token)",
        "RFC 7519 (JWT)",
        "RFC 6749 (OAuth 2.0)",
        "RFC 6265 (Cookies)",
        "RFC 9421 (HTTP Message Signatures)",
    ],
    "Compression": [
        "RFC 7932 (Brotli)",
        "RFC 8878 (Zstandard)",
        "RFC 1951 (DEFLATE)",
        "RFC 1952 (GZIP)",
        "RFC 7541 (HPACK)",
        "RFC 9204 (QPACK)",
    ],
    "Security Headers": [
        "RFC 6797 (HSTS)",
        "RFC 6962 (Certificate Transparency)",
        "RFC 9457 (Problem Details for HTTP APIs)",
    ],
    "Cryptography": [
        "RFC 8446 S4.2 (Key Exchange)",
        "RFC 8439 (ChaCha20-Poly1305)",
        "RFC 5288 (AES-GCM for TLS)",
        "RFC 7748 (X25519/X448)",
        "RFC 5869 (HKDF)",
        "RFC 5116 (AEAD)",
        "RFC 9180 (HPKE)",
    ],
}

# Relevance scoring keywords
HIGH_RELEVANCE_KEYWORDS: list[str] = [
    "http",
    "http/1.1",
    "http/2",
    "http/3",
    "quic",
    "tls 1.3",
    "websocket",
    "hpack",
    "qpack",
    "hsts",
    "x25519",
    "chacha20",
    "aes-gcm",
    "certificate",
    "aead",
    "hkdf",
    "oauth",
    "jwt",
    "bearer",
    "cookie",
    "brotli",
    "zstandard",
    "alpn",
    "sni",
    "encrypted client hello",
    "content negotiation",
    "caching",
    "compression",
    "priority",
    "early hints",
    "problem details",
    "structured field",
    "web linking",
]

MEDIUM_RELEVANCE_KEYWORDS: list[str] = [
    "json",
    "uri",
    "mime",
    "multipart",
    "base64",
    "pem",
    "pkcs",
    "congestion",
    "session resumption",
    "ocsp",
    "acme",
    "proxy",
    "server-sent",
    "rate limit",
    "client hints",
]

DRAFT_RELEVANCE_KEYWORDS: list[str] = [
    "http",
    "tls",
    "quic",
    "websocket",
    "certificate",
    "encrypt",
    "key exchange",
    "compression",
    "authentication",
    "caching",
    "priority",
]


# ── API client ────────────────────────────────────────────────────────


def _create_session() -> requests.Session:
    """Create a reusable HTTP session with connection pooling."""
    session = requests.Session()
    session.headers["User-Agent"] = USER_AGENT
    adapter = HTTPAdapter(
        max_retries=Retry(
            total=3,
            backoff_factor=0.5,
            status_forcelist=[429, 500, 502, 503, 504],
        ),
    )
    session.mount("https://", adapter)
    return session


def _parse_rfc_number(name: str) -> int | None:
    """Extract RFC number from a document name like 'rfc8446'."""
    if name.startswith("rfc"):
        try:
            return int(name[3:])
        except ValueError:
            pass
    return None


def search_datatracker_rfcs(
    session: requests.Session,
    query: str,
    *,
    max_results: int = 10,
) -> list[dict[str, Any]]:
    """Search datatracker for RFCs matching a title query."""
    params: dict[str, str | int] = {
        "title__icontains": query,
        "type": DOCTYPE_RFC,
        "limit": max_results,
        "format": "json",
        "order_by": "-rfc_number",
    }
    try:
        resp = session.get(
            f"{DATATRACKER_API}/doc/document/",
            params=params,
            timeout=REQUEST_TIMEOUT,
        )
        resp.raise_for_status()
        data = resp.json()
        results = []
        for obj in data.get("objects", []):
            rfc_num = _parse_rfc_number(obj.get("name", ""))
            if rfc_num is not None:
                results.append(
                    {
                        "rfc": rfc_num,
                        "title": obj.get("title", ""),
                        "status": obj.get("std_level", ""),
                        "pages": obj.get("pages"),
                    }
                )
        return results
    except requests.RequestException as exc:
        log.warning("Datatracker RFC search failed for %r: %s", query, exc)
        return []


def search_datatracker_drafts(
    session: requests.Session,
    query: str,
    *,
    max_results: int = 5,
) -> list[dict[str, Any]]:
    """Search datatracker for active Internet-Drafts."""
    params: dict[str, str | int] = {
        "title__icontains": query,
        "type": DOCTYPE_DRAFT,
        "states__slug__in": "active",
        "limit": max_results,
        "format": "json",
        "order_by": "-time",
    }
    try:
        resp = session.get(
            f"{DATATRACKER_API}/doc/document/",
            params=params,
            timeout=REQUEST_TIMEOUT,
        )
        resp.raise_for_status()
        data = resp.json()
        return [
            {
                "name": obj.get("name", ""),
                "title": obj.get("title", ""),
                "rev": obj.get("rev", ""),
            }
            for obj in data.get("objects", [])
        ]
    except requests.RequestException as exc:
        log.warning("Datatracker draft search failed for %r: %s", query, exc)
        return []


def fetch_rfc_metadata(
    session: requests.Session,
    rfc_numbers: list[int],
    *,
    batch_size: int = 20,
) -> dict[int, dict[str, Any]]:
    """Fetch metadata for multiple RFCs using name__in batches."""
    result: dict[int, dict[str, Any]] = {}
    for i in range(0, len(rfc_numbers), batch_size):
        batch = rfc_numbers[i : i + batch_size]
        names = ",".join(f"rfc{n}" for n in batch)
        try:
            resp = session.get(
                f"{DATATRACKER_API}/doc/document/",
                params={"name__in": names, "format": "json", "limit": batch_size},
                timeout=REQUEST_TIMEOUT,
            )
            resp.raise_for_status()
            for obj in resp.json().get("objects", []):
                rfc_num = _parse_rfc_number(obj.get("name", ""))
                if rfc_num is not None:
                    result[rfc_num] = {
                        "title": obj.get("title", ""),
                        "status": obj.get("std_level", ""),
                        "pages": obj.get("pages"),
                        "abstract": obj.get("abstract", ""),
                    }
        except requests.RequestException as exc:
            log.warning("Batch metadata fetch failed: %s", exc)
        time.sleep(RATE_LIMIT_DELAY)
    return result


def download_rfc_txt(
    session: requests.Session,
    rfc_num: int,
    dest_dir: Path,
) -> Path | None:
    """Download RFC plain-text to dest_dir/rfcNNNN.txt."""
    dest = dest_dir / f"rfc{rfc_num}.txt"
    if dest.exists():
        log.debug("Already exists: %s", dest)
        return dest
    url = f"{RFC_EDITOR_BASE}/rfc/rfc{rfc_num}.txt"
    try:
        resp = session.get(url, timeout=30)
        resp.raise_for_status()
        dest.write_bytes(resp.content)
        log.info("Downloaded rfc%d (%d bytes)", rfc_num, len(resp.content))
        return dest
    except requests.RequestException as exc:
        log.warning("Failed to download rfc%d: %s", rfc_num, exc)
        return None


# ── Collection ────────────────────────────────────────────────────────


def collect_rfcs(
    session: requests.Session,
) -> tuple[dict[int, RfcEntry], dict[str, DraftEntry]]:
    """Search all categories and collect RFC + draft entries."""
    rfcs: dict[int, RfcEntry] = {}
    drafts: dict[str, DraftEntry] = {}

    for cat_name, queries in CATEGORIES.items():
        log.info("Category: %s", cat_name)

        for query in queries:
            log.debug("  Search: %s", query)

            for r in search_datatracker_rfcs(session, query):
                num = r["rfc"]
                if num not in rfcs:
                    rfcs[num] = RfcEntry(
                        number=num,
                        title=r["title"],
                        status=r.get("status", ""),
                        pages=r.get("pages"),
                    )
                rfcs[num].categories.add(cat_name)

            for d in search_datatracker_drafts(session, query):
                name = d["name"]
                if name not in drafts:
                    drafts[name] = DraftEntry(
                        name=name,
                        title=d["title"],
                        rev=d.get("rev", ""),
                    )
                drafts[name].categories.add(cat_name)

            time.sleep(RATE_LIMIT_DELAY)

    # Merge known critical RFCs
    for rfc_num, (title, status) in KNOWN_CRITICAL.items():
        if rfc_num not in rfcs:
            rfcs[rfc_num] = RfcEntry(number=rfc_num, title=title, status=status)
        rfcs[rfc_num].categories.add("Curated critical")

    return rfcs, drafts


# ── Scoring ───────────────────────────────────────────────────────────


def relevance_score(entry: RfcEntry) -> int:
    """Score an RFC's relevance to the iohttp project."""
    score = 0
    title_lower = entry.title.lower()

    for kw in HIGH_RELEVANCE_KEYWORDS:
        if kw in title_lower:
            score += 10
    for kw in MEDIUM_RELEVANCE_KEYWORDS:
        if kw in title_lower:
            score += 5

    if entry.number in KNOWN_CRITICAL:
        score += 20

    status_lower = entry.status.lower() if entry.status else ""
    if "standard" in status_lower:
        score += 3
    elif "proposed" in status_lower:
        score += 2

    return score


# ── Markdown generation ──────────────────────────────────────────────


def _status_label(rfc_num: int, entry: RfcEntry) -> str:
    """Resolve display status: prefer curated, fall back to API."""
    if rfc_num in KNOWN_CRITICAL:
        return KNOWN_CRITICAL[rfc_num][1]
    status = entry.status
    if isinstance(status, str) and "/" in status:
        return status.rsplit("/", maxsplit=1)[-1].strip()
    return status or ""


def generate_markdown(rfcs: dict[int, RfcEntry], drafts: dict[str, DraftEntry]) -> str:
    """Generate the full Markdown registry."""
    now = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    lines: list[str] = []
    w = lines.append

    w("# iohttp HTTP Server — RFC Registry\n")
    w(f"**Generated**: {now}")
    w(f"**RFCs found**: {len(rfcs)}")
    w(f"**Active Internet-Drafts found**: {len(drafts)}")
    w("")

    # Statistics
    status_counts: dict[str, int] = defaultdict(int)
    for entry in rfcs.values():
        label = (entry.status or "UNKNOWN").upper()
        if "/" in label:
            label = label.rsplit("/", maxsplit=1)[-1].strip()
        status_counts[label] += 1

    w("---\n")
    w("## Statistics\n")
    for status, count in sorted(status_counts.items(), key=lambda x: -x[1]):
        w(f"- **{status}**: {count}")
    w("")

    # Critical RFCs
    w("---\n")
    w("## Critical RFCs (must-implement)\n")

    for group_name, rfc_nums in CRITICAL_GROUPS.items():
        w(f"\n### {group_name}\n")
        w("| RFC | Title | Status | Relevance |")
        w("|-----|-------|--------|-----------|")
        for num in rfc_nums:
            entry = rfcs.get(num)
            title = entry.title if entry else KNOWN_CRITICAL.get(num, ("?",))[0]
            status = (
                _status_label(num, entry)
                if entry
                else KNOWN_CRITICAL.get(num, ("", ""))[1]
            )
            score = relevance_score(entry) if entry else 0
            w(
                f"| [{num}]({RFC_EDITOR_BASE}/rfc/rfc{num}) | {title} | {status} | {score} |"
            )

    # High-relevance non-critical RFCs
    critical_set: set[int] = set()
    for nums in CRITICAL_GROUPS.values():
        critical_set.update(nums)

    scored = sorted(
        ((relevance_score(e), e) for e in rfcs.values()),
        key=lambda x: (-x[0], x[1].number),
    )

    w("\n---\n")
    w("## High-relevance RFCs (not in critical list)\n")
    w("| RFC | Title | Status | Score |")
    w("|-----|-------|--------|-------|")
    count = 0
    for score, entry in scored:
        if entry.number in critical_set or score < 5:
            continue
        status = _status_label(entry.number, entry)
        w(
            f"| [{entry.number}]({entry.url}) | {entry.title[:80]} | {status} | {score} |"
        )
        count += 1
        if count >= 60:
            break

    # Important drafts
    w("\n---\n")
    w("## Active Internet-Drafts\n")
    w("| Draft | Description | Status |")
    w("|-------|-------------|--------|")
    for name, desc in IMPORTANT_DRAFTS.items():
        w(f"| [{name}]({DATATRACKER_BASE}/doc/{name}/) | {desc} | Curated |")

    # Discovered drafts
    seen = set(IMPORTANT_DRAFTS.keys())
    draft_scored: list[tuple[int, DraftEntry]] = []
    for entry in drafts.values():
        base = (
            entry.name.rsplit("-", maxsplit=1)[0]
            if entry.name[-1:].isdigit()
            else entry.name
        )
        if base in seen or entry.name in seen:
            continue
        seen.add(base)
        t_lower = entry.title.lower()
        dscore = sum(10 for kw in DRAFT_RELEVANCE_KEYWORDS if kw in t_lower)
        if dscore >= 10:
            draft_scored.append((dscore, entry))

    if draft_scored:
        draft_scored.sort(key=lambda x: -x[0])
        w("\n### Additional discovered drafts\n")
        w("| Draft | Title | Categories |")
        w("|-------|-------|------------|")
        for _, entry in draft_scored[:30]:
            cats = ", ".join(sorted(entry.categories)) if entry.categories else "-"
            w(f"| [{entry.name}]({entry.url}) | {entry.title[:80]} | {cats} |")

    # Protocol matrix
    w("\n---\n")
    w("## Protocol-to-RFC matrix for iohttp\n")
    for component, rfc_refs in PROTOCOL_MATRIX.items():
        w(f"\n### {component}\n")
        for ref in rfc_refs:
            w(f"- {ref}")

    return "\n".join(lines)


def generate_json(rfcs: dict[int, RfcEntry], drafts: dict[str, DraftEntry]) -> str:
    """Generate JSON output."""
    data = {
        "generated": datetime.now(tz=timezone.utc).isoformat(),
        "rfcs": {
            num: {
                "title": e.title,
                "status": e.status,
                "pages": e.pages,
                "categories": sorted(e.categories),
                "relevance": relevance_score(e),
                "url": e.url,
            }
            for num, e in sorted(rfcs.items())
        },
        "drafts": {
            e.name: {
                "title": e.title,
                "rev": e.rev,
                "categories": sorted(e.categories),
                "url": e.url,
            }
            for e in sorted(drafts.values(), key=lambda x: x.name)
        },
    }
    return json.dumps(data, indent=2, ensure_ascii=False)


# ── CLI ───────────────────────────────────────────────────────────────


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="RFC scraper for the iohttp HTTP server project",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        dest="json_output",
        help="Output as JSON instead of Markdown",
    )
    parser.add_argument(
        "--download",
        type=Path,
        default=None,
        metavar="DIR",
        help="Download RFC .txt files to DIR (only critical RFCs)",
    )
    parser.add_argument(
        "--download-all",
        type=Path,
        default=None,
        metavar="DIR",
        help="Download ALL discovered RFC .txt files to DIR",
    )
    parser.add_argument(
        "--skip-search",
        action="store_true",
        help="Skip API search, only use curated KNOWN_CRITICAL list",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="count",
        default=0,
        help="Increase verbosity (-v info, -vv debug)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    # Configure logging
    level = logging.WARNING
    if args.verbose >= 2:
        level = logging.DEBUG
    elif args.verbose >= 1:
        level = logging.INFO
    logging.basicConfig(
        level=level,
        format="%(levelname)-5s %(message)s",
        stream=sys.stderr,
    )

    session = _create_session()

    # Collect RFCs
    if args.skip_search:
        log.info("Skipping API search, using curated list only")
        rfcs: dict[int, RfcEntry] = {}
        for rfc_num, (title, status) in KNOWN_CRITICAL.items():
            rfcs[rfc_num] = RfcEntry(
                number=rfc_num,
                title=title,
                status=status,
                categories={"Curated critical"},
            )
        drafts: dict[str, DraftEntry] = {}
    else:
        log.info("Scanning datatracker.ietf.org...")
        rfcs, drafts = collect_rfcs(session)

    log.info("Found %d RFCs and %d drafts", len(rfcs), len(drafts))

    # Download RFC text files
    download_dir = args.download_all or args.download
    if download_dir is not None:
        download_dir.mkdir(parents=True, exist_ok=True)
        if args.download_all:
            nums_to_download = sorted(rfcs.keys())
        else:
            nums_to_download = sorted(KNOWN_CRITICAL.keys())
        log.info("Downloading %d RFCs to %s", len(nums_to_download), download_dir)
        ok = 0
        for rfc_num in nums_to_download:
            if download_rfc_txt(session, rfc_num, download_dir):
                ok += 1
            time.sleep(RATE_LIMIT_DELAY)
        log.info("Downloaded %d/%d RFC files", ok, len(nums_to_download))

    # Generate output
    if args.json_output:
        output = generate_json(rfcs, drafts)
    else:
        output = generate_markdown(rfcs, drafts)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
        log.info("Written to %s (%d chars)", args.output, len(output))
    else:
        sys.stdout.write(output)
        sys.stdout.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
