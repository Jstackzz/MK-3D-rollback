"""Create the server's self-signed TLS certificate and print the SHA-256 fingerprint to pin.

    python -m server.makecert [--out-dir DIR] [--host lobby.example.org] [--host 203.0.113.7] [--days 3650]

Writes server-cert.pem and server-key.pem (RSA 2048, SHA-256 signature). Uses the
`cryptography` package when it is installed (python -m pip install --user cryptography) and
falls back to the `openssl` command-line tool otherwise.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import ipaddress
import os
import shutil
import ssl
import subprocess
import sys
from pathlib import Path

DEFAULT_CERT = "server-cert.pem"
DEFAULT_KEY = "server-key.pem"


def format_fingerprint(hex_digest: str) -> str:
    hex_digest = hex_digest.upper()
    return ":".join(hex_digest[i : i + 2] for i in range(0, len(hex_digest), 2))


def normalize_fingerprint(text: str) -> str:
    """'AB:cd 01...' -> 'abcd01...' (what the launcher and test client compare)."""
    return "".join(ch for ch in text.lower() if ch in "0123456789abcdef")


def fingerprint_of_der(der: bytes) -> str:
    return format_fingerprint(hashlib.sha256(der).hexdigest())


def fingerprint_of_pem(path: str | os.PathLike[str]) -> str:
    pem = Path(path).read_text(encoding="ascii")
    start = pem.index("-----BEGIN CERTIFICATE-----")
    end = pem.index("-----END CERTIFICATE-----") + len("-----END CERTIFICATE-----")
    return fingerprint_of_der(ssl.PEM_cert_to_DER_cert(pem[start:end] + "\n"))


def _split_hosts(hosts: list[str]) -> tuple[list[str], list[str]]:
    names, ips = ["localhost"], ["127.0.0.1"]
    for h in hosts:
        try:
            ip = str(ipaddress.ip_address(h))
        except ValueError:
            if h not in names:
                names.append(h)
        else:
            if ip not in ips:
                ips.append(ip)
    return names, ips


def generate_with_cryptography(cert_path: Path, key_path: Path, common_name: str, days: int,
                               hosts: list[str]) -> None:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name),
                         x509.NameAttribute(NameOID.ORGANIZATION_NAME, "MK3D Online")])
    names, ips = _split_hosts(hosts)
    san = [x509.DNSName(n) for n in names] + [x509.IPAddress(ipaddress.ip_address(i)) for i in ips]
    now = dt.datetime.now(dt.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(days=1))
        .not_valid_after(now + dt.timedelta(days=days))
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(key, hashes.SHA256())
    )
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))


def generate_with_openssl(cert_path: Path, key_path: Path, common_name: str, days: int, hosts: list[str]) -> None:
    exe = shutil.which("openssl")
    if exe is None:
        raise RuntimeError("neither the 'cryptography' package nor the 'openssl' tool is available; "
                           "install one: python -m pip install --user cryptography")
    names, ips = _split_hosts(hosts)
    san = ",".join([f"DNS:{n}" for n in names] + [f"IP:{i}" for i in ips])
    subprocess.run(
        [exe, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", str(days),
         "-keyout", str(key_path), "-out", str(cert_path), "-subj", f"/CN={common_name}/O=MK3D Online",
         "-addext", f"subjectAltName={san}", "-addext", "basicConstraints=critical,CA:FALSE"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )


def generate(cert_path: str | os.PathLike[str], key_path: str | os.PathLike[str], *, common_name: str = "mk3d-server",
             days: int = 3650, hosts: list[str] | None = None) -> str:
    """Write a new key and self-signed certificate; returns the colon-separated SHA-256 fingerprint."""
    cert_p, key_p = Path(cert_path), Path(key_path)
    cert_p.parent.mkdir(parents=True, exist_ok=True)
    key_p.parent.mkdir(parents=True, exist_ok=True)
    try:
        import cryptography  # noqa: F401
    except ImportError:
        generate_with_openssl(cert_p, key_p, common_name, days, hosts or [])
    else:
        generate_with_cryptography(cert_p, key_p, common_name, days, hosts or [])
    try:
        os.chmod(key_p, 0o600)
    except OSError:
        pass
    return fingerprint_of_pem(cert_p)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m server.makecert", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", default=".", help="folder for the two files (default: current folder)")
    ap.add_argument("--cert", default=DEFAULT_CERT, help=f"certificate file name (default {DEFAULT_CERT})")
    ap.add_argument("--key", default=DEFAULT_KEY, help=f"private key file name (default {DEFAULT_KEY})")
    ap.add_argument("--cn", default="mk3d-server", help="certificate common name")
    ap.add_argument("--host", action="append", default=[],
                    help="DNS name or IP to put in subjectAltName (repeatable; informational, the launcher pins)")
    ap.add_argument("--days", type=int, default=3650, help="validity in days (default 3650)")
    ap.add_argument("--force", action="store_true", help="overwrite existing files")
    ap.add_argument("--show", action="store_true", help="only print the fingerprint of an existing certificate")
    args = ap.parse_args(argv)

    out = Path(args.out_dir)
    cert_path, key_path = out / args.cert, out / args.key
    if args.show:
        print(fingerprint_of_pem(cert_path))
        return 0
    if (cert_path.exists() or key_path.exists()) and not args.force:
        print(f"{cert_path} or {key_path} already exists; use --force to replace them "
              f"(launchers that pinned the old fingerprint will refuse the new one).", file=sys.stderr)
        print(f"current fingerprint: {fingerprint_of_pem(cert_path)}" if cert_path.exists() else "", file=sys.stderr)
        return 1
    fp = generate(cert_path, key_path, common_name=args.cn, days=args.days, hosts=args.host)
    print(f"certificate: {cert_path.resolve()}")
    print(f"private key: {key_path.resolve()} (keep it secret)")
    print(f"SHA-256 fingerprint: {fp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
