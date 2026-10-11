"""Local OIDC provider with real RSA signatures; no external identity service."""

import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlencode, urlsplit


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


class OIDCProvider:
    secret = "oidc-fixture-client-secret"
    subject = "fixture-existing-subject"

    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix="punto-oidc-provider-")
        self.directory = Path(self.temp.name)
        self.openssl = shutil.which("openssl")
        if not self.openssl:
            raise RuntimeError("OpenSSL required for OIDC fixture")
        self.key = self.directory / "signing.key"
        subprocess.run([self.openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(self.key)],
                       check=True, capture_output=True, timeout=15)
        modulus = subprocess.run([self.openssl, "rsa", "-in", str(self.key),
                                  "-noout", "-modulus"], check=True,
                                 capture_output=True, text=True, timeout=5)
        self.jwk = {"kty": "RSA", "kid": "fixture-key", "use": "sig", "alg": "RS256",
                    "n": b64(bytes.fromhex(modulus.stdout.strip().split("=", 1)[1])),
                    "e": "AQAB"}
        self.codes = {}
        self.token_requests = []
        self.jwks_requests = 0
        provider = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def respond(self, status, body):
                body = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path != "/realms/punto/protocol/openid-connect/certs":
                    self.respond(404, {})
                    return
                provider.jwks_requests += 1
                self.respond(200, {"keys": [provider.jwk]})

            def do_POST(self):
                if self.path != "/realms/punto/protocol/openid-connect/token":
                    self.respond(404, {})
                    return
                data = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
                provider.token_requests.append(data)
                code = data.get("code", [""])[0]
                transaction = provider.codes.pop(code, None)
                if not transaction:
                    self.respond(400, {"error": "invalid_grant"})
                    return
                authorize, claims, header, bad_signature, extra = transaction
                verifier = data.get("code_verifier", [""])[0]
                valid = (data.get("grant_type") == ["authorization_code"] and
                         data.get("client_secret") == [provider.secret] and
                         data.get("client_id") == authorize.get("client_id") and
                         data.get("redirect_uri") == authorize.get("redirect_uri") and
                         b64(hashlib.sha256(verifier.encode()).digest()) == authorize["code_challenge"][0])
                if not valid:
                    self.respond(400, {"error": "invalid_request"})
                    return
                now = int(time.time())
                payload = {"iss": provider.issuer, "aud": authorize["client_id"][0],
                           "azp": authorize["client_id"][0], "sub": provider.subject,
                           "nonce": authorize["nonce"][0], "iat": now, "exp": now + 300,
                           "email": "untrusted-claim@example.test", "realm_access": {"roles": ["admin"]}}
                payload.update(claims)
                jwt = provider.sign(payload, header, bad_signature)
                body = {"id_token": jwt, "token_type": "Bearer", "expires_in": 300}
                body.update(extra)
                self.respond(200, body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.issuer = f"http://127.0.0.1:{self.server.server_port}/realms/punto"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def sign(self, payload, header=None, bad_signature=False):
        signing_header = {"alg": "RS256", "kid": "fixture-key", "typ": "JWT"}
        signing_header.update(header or {})
        data = (b64(json.dumps(signing_header).encode()) + "." + b64(json.dumps(payload).encode())).encode()
        signature = subprocess.run([self.openssl, "dgst", "-sha256", "-sign", str(self.key)],
                                   input=data, check=True, capture_output=True, timeout=5).stdout
        if bad_signature:
            signature = bytes([signature[0] ^ 1]) + signature[1:]
        return data.decode() + "." + b64(signature)

    def register(self, location, claims=None, header=None, bad_signature=False, extra_token=None):
        """Register an authorization redirect and return a callback query string."""
        authorize = parse_qs(urlsplit(location).query)
        code = secrets.token_urlsafe(24)
        self.codes[code] = (authorize, claims or {}, header or {}, bad_signature, extra_token or {})
        return urlencode({"code": code, "state": authorize["state"][0], "iss": self.issuer,
                          "session_state": "fixture-session"})

    def configure(self, directory, redirect_uri, client_id="punto", bindings=None, mode="hybrid"):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        secret = directory / f"{client_id}-secret"
        secret.write_text(self.secret)
        secret.chmod(0o600)
        mapping = directory / f"{client_id}-bindings.json"
        mapping.write_text(json.dumps(bindings if bindings is not None else {self.subject: "oidc-local@example.test"}))
        mapping.chmod(0o600)
        return {"AUTH_MODE": mode, "OIDC_ISSUER_URL": self.issuer,
                "OIDC_BACKCHANNEL_URL": self.issuer, "OIDC_CLIENT_ID": client_id,
                "OIDC_CLIENT_SECRET_FILE": str(secret), "OIDC_BINDINGS_FILE": str(mapping),
                "OIDC_REDIRECT_URI": redirect_uri, "OIDC_ALLOW_HTTP": "1"}

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
