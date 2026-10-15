# SPDX-License-Identifier: Apache-2.0
"""Android APKs made from a runtime template without the Android SDK or a JDK.

The template's base.apk (tools/make_template.py) is complete except for the game. Exporting:

1. patches the compiled AndroidManifest.xml: the placeholder package name, title and version
   strings are replaced in its string pool, and android:versionCode is set;
2. writes a new zip with the game package under assets/data/, keeping resources.arsc stored and
   4-byte aligned (Android 11+ refuses to install anything else);
3. signs it with APK Signature Scheme v2, which every Android version Midgard supports
   (10 and later) verifies.

The signing key is the editor's development key (<settings>/android/), created on first use.
Keep a copy: an installed game only accepts updates signed with the same key.
"""
import datetime
import hashlib
import io
import re
import struct
import zipfile
from dataclasses import dataclass
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID

PLACEHOLDER_PACKAGE = "org.oldgoatstudio.midgard.template"
PLACEHOLDER_TITLE = "@@MIDGARD_GAME_TITLE@@"
PLACEHOLDER_VERSION = "@@MIDGARD_GAME_VERSION@@"
PACKAGE_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+")

# Binary XML (AXML) chunk types
RES_STRING_POOL_TYPE = 0x0001
RES_XML_TYPE = 0x0003
RES_XML_START_ELEMENT_TYPE = 0x0102
UTF8_FLAG = 0x100
TYPE_INT_DEC = 0x10

# APK Signature Scheme v2
APK_SIG_BLOCK_MAGIC = b"APK Sig Block 42"
APK_SIGNATURE_SCHEME_V2_ID = 0x7109871A
RSA_PKCS1_V1_5_WITH_SHA256 = 0x0103
CHUNK_SIZE = 1024 * 1024
EOCD_SIGNATURE = 0x06054B50

FIXED_DATE = (1980, 1, 1, 0, 0, 0)  # reproducible exports


class ApkError(Exception):
    """The APK can't be made. The message is shown to the user."""


def valid_package_name(name: str) -> bool:
    return PACKAGE_RE.fullmatch(name) is not None


def version_code(version: str) -> int:
    """'1.2.3' -> 1002003: later versions always get larger codes, as Android requires."""
    parts = [int(n) for n in re.findall(r"\d+", version)[:3]] + [0, 0, 0]
    return max(1, parts[0] * 1_000_000 + parts[1] * 1_000 + parts[2])


# ---- binary XML: AndroidManifest.xml ----------------------------------------------------------
def _decode_length(data: bytes, pos: int, utf8: bool) -> tuple[int, int]:
    if utf8:
        n = data[pos]
        if n & 0x80:
            return ((n & 0x7F) << 8) | data[pos + 1], pos + 2
        return n, pos + 1
    n = struct.unpack_from("<H", data, pos)[0]
    if n & 0x8000:
        return ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, pos + 2)[0], pos + 4
    return n, pos + 2


def _encode_length(n: int, utf8: bool) -> bytes:
    if utf8:
        return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])
    return struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16),
                                                                 n & 0xFFFF)


def _read_pool(data: bytes, start: int) -> tuple[list[str], dict]:
    (_, header_size, size, count, style_count, flags, strings_start,
     styles_start) = struct.unpack_from("<HHIIIIII", data, start)
    utf8 = bool(flags & UTF8_FLAG)
    offsets = struct.unpack_from(f"<{count}I", data, start + header_size)
    strings = []
    for off in offsets:
        pos = start + strings_start + off
        if utf8:
            _, pos = _decode_length(data, pos, True)  # length in UTF-16 units
            n, pos = _decode_length(data, pos, True)  # length in bytes
            strings.append(data[pos:pos + n].decode("utf-8"))
        else:
            n, pos = _decode_length(data, pos, False)
            strings.append(data[pos:pos + 2 * n].decode("utf-16-le"))
    pool = {"start": start, "header_size": header_size, "size": size, "flags": flags,
            "style_count": style_count, "strings_start": strings_start,
            "styles_start": styles_start, "utf8": utf8}
    return strings, pool


def _write_pool(strings: list[str], pool: dict, data: bytes) -> bytes:
    utf8 = pool["utf8"]
    body = bytearray()
    offsets = []
    for s in strings:
        offsets.append(len(body))
        if utf8:
            raw = s.encode("utf-8")
            body += _encode_length(len(s.encode("utf-16-le")) // 2, True)
            body += _encode_length(len(raw), True) + raw + b"\0"
        else:
            raw = s.encode("utf-16-le")
            body += _encode_length(len(raw) // 2, False) + raw + b"\0\0"
    body += b"\0" * (-len(body) % 4)
    start, style_count = pool["start"], pool["style_count"]
    style_offsets = b""
    styles = b""
    if style_count:  # keep styled-text spans as they are (a manifest has none)
        first = start + pool["header_size"] + 4 * len(strings)
        style_offsets = data[first:first + 4 * style_count]
        styles = data[start + pool["styles_start"]:start + pool["size"]]
    header_size = pool["header_size"]
    strings_start = header_size + 4 * len(strings) + 4 * style_count
    styles_start = strings_start + len(body) if style_count else 0
    size = strings_start + len(body) + len(styles)
    header = struct.pack("<HHIIIIII", RES_STRING_POOL_TYPE, header_size, size, len(strings),
                         style_count, pool["flags"], strings_start, styles_start)
    header += b"\0" * (header_size - len(header))
    offset_table = struct.pack(f"<{len(strings)}I", *offsets)
    return header + offset_table + style_offsets + bytes(body) + styles


def manifest_strings(axml: bytes) -> list[str]:
    strings, _ = _read_pool(axml, _pool_start(axml))
    return strings


def _pool_start(axml: bytes) -> int:
    kind, header_size, _ = struct.unpack_from("<HHI", axml, 0)
    if kind != RES_XML_TYPE:
        raise ApkError("AndroidManifest.xml in the template is not compiled binary XML")
    if struct.unpack_from("<H", axml, header_size)[0] != RES_STRING_POOL_TYPE:
        raise ApkError("AndroidManifest.xml in the template has no string pool")
    return header_size


def int_attributes(axml: bytes, name: str) -> list[int]:
    """Values of every integer attribute with this name (for checks and tests)."""
    return [struct.unpack_from("<I", axml, pos)[0] for pos in _int_attribute_offsets(axml, name)]


def _int_attribute_offsets(axml: bytes, name: str) -> list[int]:
    strings, _ = _read_pool(axml, _pool_start(axml))
    found = []
    pos = struct.unpack_from("<H", axml, 2)[0]
    while pos < len(axml):
        kind, header_size, size = struct.unpack_from("<HHI", axml, pos)
        if kind == RES_XML_START_ELEMENT_TYPE:
            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", axml,
                                                                   pos + header_size + 8)
            for i in range(attr_count):
                a = pos + header_size + attr_start + i * attr_size
                name_index = struct.unpack_from("<I", axml, a + 4)[0]
                data_type = axml[a + 15]
                if name_index < len(strings) and strings[name_index] == name \
                        and data_type == TYPE_INT_DEC:
                    found.append(a + 16)
        pos += size
    return found


def patch_manifest(axml: bytes, replacements: dict[str, str],
                   int_values: dict[str, int] | None = None) -> bytes:
    """Replaces whole strings in the manifest's string pool and sets integer attributes."""
    start = _pool_start(axml)
    strings, pool = _read_pool(axml, start)
    new_pool = _write_pool([replacements.get(s, s) for s in strings], pool, axml)
    out = bytearray(axml[:start] + new_pool + axml[start + pool["size"]:])
    struct.pack_into("<I", out, 4, len(out))  # the document's total size
    for name, value in (int_values or {}).items():
        for offset in _int_attribute_offsets(bytes(out), name):
            struct.pack_into("<I", out, offset, value)
    return bytes(out)


# ---- the zip ----------------------------------------------------------------------------------
@dataclass
class Entry:
    name: str
    data: bytes
    compress: bool = True


def write_zip(entries: list[Entry], alignment: int = 4) -> bytes:
    """A zip in which every stored entry's data starts on an `alignment`-byte boundary."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for e in entries:
            info = zipfile.ZipInfo(e.name, FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED if e.compress else zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            if not e.compress:
                # Pad with the Android alignment extra field (0xd935), as zipalign does.
                offset = buf.tell() + 30 + len(e.name.encode("utf-8")) + 6
                pad = -offset % alignment
                info.extra = struct.pack("<HHH", 0xD935, 2 + pad, alignment) + b"\0" * pad
            z.writestr(info, e.data)
    return buf.getvalue()


def _eocd(apk: bytes) -> int:
    pos = apk.rfind(struct.pack("<I", EOCD_SIGNATURE), max(0, len(apk) - 65557))
    if pos < 0:
        raise ApkError("not a zip file")
    return pos


# ---- APK Signature Scheme v2 ------------------------------------------------------------------
def _lp(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def _content_digest(sections: list[bytes]) -> bytes:
    chunks = []
    for section in sections:
        for i in range(0, len(section), CHUNK_SIZE):
            chunk = section[i:i + CHUNK_SIZE]
            chunks.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def _sections(apk: bytes, cd_offset: int, eocd: int) -> list[bytes]:
    return [apk[:cd_offset], apk[cd_offset:eocd], apk[eocd:]]


@dataclass
class SigningKey:
    private_key: rsa.RSAPrivateKey
    certificate: x509.Certificate


def sign_v2(apk: bytes, key: SigningKey) -> bytes:
    """Inserts an APK Signing Block with a v2 signature before the central directory."""
    eocd = _eocd(apk)
    cd_size, cd_offset = struct.unpack_from("<II", apk, eocd + 12)
    digest = _content_digest(_sections(apk, cd_offset, eocd))
    cert = key.certificate.public_bytes(serialization.Encoding.DER)
    public = key.private_key.public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signed_data = (_lp(_lp(struct.pack("<I", RSA_PKCS1_V1_5_WITH_SHA256) + _lp(digest)))
                   + _lp(_lp(cert)) + _lp(b""))
    signature = key.private_key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    signer = (_lp(signed_data)
              + _lp(_lp(struct.pack("<I", RSA_PKCS1_V1_5_WITH_SHA256) + _lp(signature)))
              + _lp(public))
    value = _lp(_lp(signer))
    pair = struct.pack("<QI", 4 + len(value), APK_SIGNATURE_SCHEME_V2_ID) + value
    block_size = len(pair) + 8 + len(APK_SIG_BLOCK_MAGIC)
    block = (struct.pack("<Q", block_size) + pair + struct.pack("<Q", block_size)
             + APK_SIG_BLOCK_MAGIC)
    new_eocd = bytearray(apk[eocd:])
    struct.pack_into("<I", new_eocd, 16, cd_offset + len(block))
    return apk[:cd_offset] + block + apk[cd_offset:eocd] + bytes(new_eocd)


def verify_v2(apk: bytes) -> x509.Certificate:
    """Checks the v2 signature (as Android does) and returns the signer's certificate."""
    from cryptography.exceptions import InvalidSignature

    eocd = _eocd(apk)
    cd_offset = struct.unpack_from("<I", apk, eocd + 16)[0]
    if apk[cd_offset - 16:cd_offset] != APK_SIG_BLOCK_MAGIC:
        raise ApkError("the APK is not signed")
    block_size = struct.unpack_from("<Q", apk, cd_offset - 24)[0]
    block_start = cd_offset - block_size - 8
    pos, end = block_start + 8, cd_offset - 24
    value = None
    while pos < end:
        length, pair_id = struct.unpack_from("<QI", apk, pos)
        if pair_id == APK_SIGNATURE_SCHEME_V2_ID:
            value = apk[pos + 12:pos + 8 + length]
        pos += 8 + length
    if value is None:
        raise ApkError("the APK has no v2 signature")

    def items(data: bytes) -> list[bytes]:
        out, i = [], 0
        while i < len(data):
            n = struct.unpack_from("<I", data, i)[0]
            out.append(data[i + 4:i + 4 + n])
            i += 4 + n
        return out

    signer = items(items(value)[0])[0]
    signed_data, signatures, public = items(signer)
    digests, certs, _ = items(signed_data)
    certificate = x509.load_der_x509_certificate(items(certs)[0])
    signature = items(items(signatures)[0][4:])[0]  # uint32 algorithm, then the signature
    public_key = serialization.load_der_public_key(public)
    try:
        public_key.verify(signature, signed_data, padding.PKCS1v15(), hashes.SHA256())
    except InvalidSignature as e:
        raise ApkError("the APK signature does not match") from e
    eocd_copy = bytearray(apk[eocd:])
    struct.pack_into("<I", eocd_copy, 16, block_start)
    digest = _content_digest([apk[:block_start], apk[cd_offset:eocd], bytes(eocd_copy)])
    if items(items(digests)[0][4:])[0] != digest:
        raise ApkError("the APK contents do not match its signature")
    return certificate


# ---- the development key ----------------------------------------------------------------------
def development_key(settings_dir: Path) -> tuple[SigningKey, Path]:
    """The editor's signing key, created on first use. Returns it and its folder."""
    folder = settings_dir / "android"
    key_file, cert_file = folder / "midgard-dev-key.pem", folder / "midgard-dev-cert.pem"
    if key_file.is_file() and cert_file.is_file():
        private_key = serialization.load_pem_private_key(key_file.read_bytes(), password=None)
        certificate = x509.load_pem_x509_certificate(cert_file.read_bytes())
        return SigningKey(private_key, certificate), folder
    folder.mkdir(parents=True, exist_ok=True)
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Midgard development key")])
    now = datetime.datetime.now(datetime.UTC)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                   .public_key(private_key.public_key())
                   .serial_number(x509.random_serial_number())
                   .not_valid_before(now - datetime.timedelta(days=1))
                   .not_valid_after(now + datetime.timedelta(days=365 * 30))
                   .sign(private_key, hashes.SHA256()))
    key_file.write_bytes(private_key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    cert_file.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    return SigningKey(private_key, certificate), folder


# ---- the game APK -----------------------------------------------------------------------------
def build_apk(base_apk: Path, data_dir: Path, extra_assets: dict[str, bytes], package: str,
              title: str, version: str, key: SigningKey) -> bytes:
    """The signed game APK: the template plus the cooked package in assets/data/."""
    if not valid_package_name(package):
        raise ApkError(f"'{package}' is not a valid Android package name "
                       "(for example com.yourstudio.yourgame)")
    entries: list[Entry] = []
    try:
        with zipfile.ZipFile(base_apk) as z:
            for info in z.infolist():
                if info.is_dir() or info.filename.startswith("META-INF/"):
                    continue
                data = z.read(info)
                if info.filename == "AndroidManifest.xml":
                    data = patch_manifest(
                        data, {PLACEHOLDER_PACKAGE: package, PLACEHOLDER_TITLE: title,
                               PLACEHOLDER_VERSION: version},
                        {"versionCode": version_code(version)})
                entries.append(Entry(info.filename, data,
                                     compress=info.filename != "resources.arsc"))
    except (zipfile.BadZipFile, KeyError) as e:
        raise ApkError(f"the template's {base_apk.name} is damaged: {e}") from e
    if not any(e.name == "AndroidManifest.xml" for e in entries):
        raise ApkError(f"the template's {base_apk.name} has no AndroidManifest.xml")
    for path in sorted(data_dir.rglob("*")):
        if path.is_file():
            entries.append(Entry(f"assets/data/{path.relative_to(data_dir).as_posix()}",
                                 path.read_bytes()))
    for name, data in extra_assets.items():
        entries.append(Entry(f"assets/{name}", data))
    return sign_v2(write_zip(entries), key)
