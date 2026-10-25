#!/usr/bin/env python3
"""Generate a disposable Let's Encrypt-style ECDSA X.509 chain for Flowie tests.

OpenSSL here is ONLY an independent test-PKI constructor and reference oracle.
Flowie's tested runtime remains CNet with its private GmSSL provider.
Private keys stay in the test binary directory and are never checked in.
"""
import argparse
import base64
import os
from pathlib import Path
import subprocess
import textwrap

def openssl(exe, *args, expect=0):
    p = subprocess.run([exe, *map(str, args)], capture_output=True, text=True)
    if (p.returncode == 0) != (expect == 0):
        raise RuntimeError("openssl %s: exit %d, expected %s: %s" %
                           (args[0], p.returncode, expect, (p.stdout + p.stderr)[-1100:]))
    return p.stdout + p.stderr

def create(exe, folder):
    folder.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(folder, 0o700)
    except OSError:
        pass
    root = folder / "ca.pem"
    root_key = folder / "ca.key"
    wrong = folder / "wrong-ca.pem"
    wrong_key = folder / "wrong-ca.key"
    leaf = folder / "leaf.pem"
    dns_only_leaf = folder / "dns-only-leaf.pem"
    intermediate = folder / "intermediate.pem"
    intermediate_key = folder / "intermediate.key"
    intermediate_request = folder / "intermediate.csr"
    intermediate_extensions = folder / "intermediate.ext"
    intermediate_leaf = folder / "intermediate-leaf.pem"
    intermediate_chain = folder / "intermediate-chain.pem"
    invalid_basic_ca_intermediate = folder / "invalid-basic-ca-intermediate.pem"
    invalid_basic_ca_key = folder / "invalid-basic-ca-intermediate.key"
    invalid_basic_ca_request = folder / "invalid-basic-ca-intermediate.csr"
    invalid_basic_ca_extensions = folder / "invalid-basic-ca-intermediate.ext"
    invalid_basic_ca_leaf = folder / "invalid-basic-ca-leaf.pem"
    invalid_basic_ca_chain = folder / "invalid-basic-ca-chain.pem"
    invalid_key_usage_intermediate = folder / "invalid-key-usage-intermediate.pem"
    invalid_key_usage_key = folder / "invalid-key-usage-intermediate.key"
    invalid_key_usage_request = folder / "invalid-key-usage-intermediate.csr"
    invalid_key_usage_extensions = folder / "invalid-key-usage-intermediate.ext"
    invalid_key_usage_leaf = folder / "invalid-key-usage-leaf.pem"
    invalid_key_usage_chain = folder / "invalid-key-usage-chain.pem"
    invalid_eku_leaf = folder / "invalid-eku-leaf.pem"
    expired_leaf = folder / "expired-leaf.pem"
    future_leaf = folder / "future-leaf.pem"
    leaf_key = folder / "leaf.key"
    request = folder / "leaf.csr"
    extensions = folder / "leaf.ext"
    client_leaf = folder / "client-leaf.pem"
    client_key = folder / "client.key"
    client_request = folder / "client.csr"
    client_extensions = folder / "client.ext"
    tampered = folder / "tampered.pem"
    ca_config = folder / "ca.cnf"

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:secp384r1", "-out", root_key)
    openssl(exe, "req", "-new", "-x509", "-key", root_key,
            "-subj", "/CN=Flowie P384 SHA384 Test Root",
            "-days", "365", "-sha384", "-out", root,
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:1",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:prime256v1", "-out", leaf_key)
    openssl(exe, "req", "-new", "-key", leaf_key,
            "-subj", "/CN=localhost", "-out", request)
    extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", request, "-CA", root, "-CAkey", root_key,
            "-set_serial", "0x1001", "-days", "30", "-sha384",
            "-extfile", extensions, "-out", leaf)

    extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", request, "-CA", root, "-CAkey", root_key,
            "-set_serial", "0x1002", "-days", "30", "-sha384",
            "-extfile", extensions, "-out", dns_only_leaf)

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:prime256v1", "-out", client_key)
    openssl(exe, "req", "-new", "-key", client_key,
            "-subj", "/CN=flowie-client", "-out", client_request)
    client_extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=clientAuth\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", client_request, "-CA", root, "-CAkey", root_key,
            "-set_serial", "0x1003", "-days", "30", "-sha384",
            "-extfile", client_extensions, "-out", client_leaf)

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:secp384r1", "-out", intermediate_key)
    openssl(exe, "req", "-new", "-key", intermediate_key,
            "-subj", "/CN=Flowie P384 SHA384 Test Intermediate", "-out",
            intermediate_request)
    intermediate_extensions.write_text(
        "basicConstraints=critical,CA:TRUE,pathlen:0\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", intermediate_request, "-CA", root,
            "-CAkey", root_key, "-set_serial", "0x2001", "-days", "60", "-sha384",
            "-extfile", intermediate_extensions, "-out", intermediate)

    extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", request, "-CA", intermediate,
            "-CAkey", intermediate_key, "-set_serial", "0x2002", "-days", "30", "-sha384",
            "-extfile", extensions, "-out", intermediate_leaf)
    intermediate_chain.write_text(
        intermediate_leaf.read_text(encoding="ascii")
        + intermediate.read_text(encoding="ascii"), encoding="ascii")

    # Each signer violation is isolated: the leaf signatures remain valid, so
    # rejection proves CA authorization checks rather than signature failure.
    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:secp384r1", "-out", invalid_basic_ca_key)
    openssl(exe, "req", "-new", "-key", invalid_basic_ca_key,
            "-subj", "/CN=Flowie Invalid Basic Constraints Intermediate", "-out",
            invalid_basic_ca_request)
    invalid_basic_ca_extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", invalid_basic_ca_request,
            "-CA", root, "-CAkey", root_key, "-set_serial", "0x3001", "-days", "60",
            "-sha384", "-extfile", invalid_basic_ca_extensions,
            "-out", invalid_basic_ca_intermediate)
    openssl(exe, "x509", "-req", "-in", request, "-CA", invalid_basic_ca_intermediate,
            "-CAkey", invalid_basic_ca_key, "-set_serial", "0x3002", "-days", "30",
            "-sha384", "-extfile", extensions, "-out", invalid_basic_ca_leaf)
    invalid_basic_ca_chain.write_text(
        invalid_basic_ca_leaf.read_text(encoding="ascii")
        + invalid_basic_ca_intermediate.read_text(encoding="ascii"), encoding="ascii")

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:secp384r1", "-out", invalid_key_usage_key)
    openssl(exe, "req", "-new", "-key", invalid_key_usage_key,
            "-subj", "/CN=Flowie Invalid Key Usage Intermediate", "-out",
            invalid_key_usage_request)
    invalid_key_usage_extensions.write_text(
        "basicConstraints=critical,CA:TRUE,pathlen:0\n"
        "keyUsage=critical,digitalSignature\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", invalid_key_usage_request,
            "-CA", root, "-CAkey", root_key, "-set_serial", "0x4001", "-days", "60",
            "-sha384", "-extfile", invalid_key_usage_extensions,
            "-out", invalid_key_usage_intermediate)
    openssl(exe, "x509", "-req", "-in", request, "-CA", invalid_key_usage_intermediate,
            "-CAkey", invalid_key_usage_key, "-set_serial", "0x4002", "-days", "30",
            "-sha384", "-extfile", extensions, "-out", invalid_key_usage_leaf)
    invalid_key_usage_chain.write_text(
        invalid_key_usage_leaf.read_text(encoding="ascii")
        + invalid_key_usage_intermediate.read_text(encoding="ascii"), encoding="ascii")

    extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=clientAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n",
        encoding="ascii")
    openssl(exe, "x509", "-req", "-in", request, "-CA", root, "-CAkey", root_key,
            "-set_serial", "0x5001", "-days", "30", "-sha384",
            "-extfile", extensions, "-out", invalid_eku_leaf)

    # `openssl x509 -req` cannot set an absolute validity window. Use the
    # reference CA command for deterministic expired and not-yet-valid leaves.
    # These files are test inputs only and never enter Flowie's runtime graph.
    ca_dir = folder.resolve().as_posix()
    (folder / "newcerts").mkdir(exist_ok=True)
    (folder / "index.txt").write_text("", encoding="ascii")
    (folder / "serial").write_text("1003\n", encoding="ascii")
    ca_config.write_text(textwrap.dedent(f"""
        [ ca ]
        default_ca = test_ca
        [ test_ca ]
        dir = {ca_dir}
        database = $dir/index.txt
        new_certs_dir = $dir/newcerts
        certificate = $dir/ca.pem
        private_key = $dir/ca.key
        serial = $dir/serial
        default_md = sha384
        policy = test_policy
        unique_subject = no
        x509_extensions = server_leaf
        [ test_policy ]
        commonName = supplied
        [ server_leaf ]
        basicConstraints = critical,CA:FALSE
        keyUsage = critical,digitalSignature
        extendedKeyUsage = serverAuth
        subjectAltName = DNS:localhost,IP:127.0.0.1
        """).lstrip(), encoding="ascii")
    openssl(exe, "ca", "-batch", "-config", ca_config, "-in", request,
            "-out", expired_leaf, "-notext", "-startdate", "220101000000Z",
            "-enddate", "230101000000Z", "-extensions", "server_leaf")
    openssl(exe, "ca", "-batch", "-config", ca_config, "-in", request,
            "-out", future_leaf, "-notext", "-startdate", "350101000000Z",
            "-enddate", "360101000000Z", "-extensions", "server_leaf")

    openssl(exe, "genpkey", "-algorithm", "EC",
            "-pkeyopt", "ec_paramgen_curve:secp384r1", "-out", wrong_key)
    openssl(exe, "req", "-new", "-x509", "-key", wrong_key,
            "-subj", "/CN=Wrong Flowie P384 SHA384 Root",
            "-days", "365", "-sha384", "-out", wrong,
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:1",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    leaf_text = openssl(exe, "x509", "-in", leaf, "-noout", "-text")
    root_text = openssl(exe, "x509", "-in", root, "-noout", "-text")
    intermediate_text = openssl(exe, "x509", "-in", intermediate, "-noout", "-text")
    if ("ecdsa-with-SHA384" not in leaf_text
            or "prime256v1" not in leaf_text
            or "secp384r1" not in root_text
            or "secp384r1" not in intermediate_text):
        raise RuntimeError("Wrong P256 leaf/P384 issuer/SHA384 certificate geometry")

    # Flip the signatureValue tail while leaving the certificate structure
    # and public key intact: a verifier must fail its signature check.
    pem_lines = [x for x in leaf.read_text(encoding="ascii").splitlines()
                 if x and not x.startswith("-----")]
    der = bytearray(base64.b64decode("".join(pem_lines)))
    der[-1] ^= 0x01
    encoded = base64.b64encode(der).decode("ascii")
    tampered.write_text("-----BEGIN CERTIFICATE-----\n"
                        + "\n".join(textwrap.wrap(encoded, 64))
                        + "\n-----END CERTIFICATE-----\n", encoding="ascii")

    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", leaf)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslclient", client_leaf)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslclient", leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-untrusted", intermediate,
            "-purpose", "sslserver", "-verify_hostname", "localhost", intermediate_leaf)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", intermediate_leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-untrusted", invalid_basic_ca_intermediate,
            "-purpose", "sslserver", "-verify_hostname", "localhost", invalid_basic_ca_leaf,
            expect=1)
    openssl(exe, "verify", "-CAfile", root, "-untrusted", invalid_key_usage_intermediate,
            "-purpose", "sslserver", "-verify_hostname", "localhost", invalid_key_usage_leaf,
            expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", invalid_eku_leaf, expect=1)
    openssl(exe, "verify", "-CAfile", wrong, "-purpose", "sslserver",
            "-verify_hostname", "localhost", leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "wrong.example", leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_ip", "127.0.0.1", dns_only_leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", tampered, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", expired_leaf, expect=1)
    openssl(exe, "verify", "-CAfile", root, "-purpose", "sslserver",
            "-verify_hostname", "localhost", future_leaf, expect=1)
    print("P384 root/intermediate/P256 leaf SHA384 test-only PKI and fail-closed oracle: PASS")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    create(args.openssl, args.out)
