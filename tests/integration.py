"""Loopback-only SSH server; uses generated, disposable test keys."""
import argparse
import pathlib
import socket
import subprocess
import tempfile
import threading

import paramiko
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa


class Server(paramiko.ServerInterface):
    def __init__(self, mode, expected_key):
        self.mode = mode
        self.expected_key = expected_key
        self.auth_calls = 0
        self.shell = threading.Event()
        self.resized = threading.Event()
        self.dimensions = None

    def get_allowed_auths(self, username):
        return "publickey,password"

    def check_auth_password(self, username, password):
        self.auth_calls += 1
        if not self.mode.startswith(("rsa", "ec")) and username == "test" and password == "correct":
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_auth_publickey(self, username, key):
        self.auth_calls += 1
        if self.mode.startswith(("rsa", "ec")) and username == "test" and key.asbytes() == self.expected_key.asbytes():
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, channel, term, width, height, pixelwidth, pixelheight, modes):
        self.dimensions = (width, height)
        return True

    def check_channel_shell_request(self, channel):
        self.shell.set()
        return True

    def check_channel_window_change_request(self, channel, width, height, pixelwidth, pixelheight):
        self.dimensions = (width, height)
        self.resized.set()
        return True


def run_case(executable, mode, key_path, expected_key, host_key):
    server = Server(mode, expected_key)
    failures = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(35)
        port = listener.getsockname()[1]

        def serve():
            transport = None
            try:
                connection, _ = listener.accept()
                transport = paramiko.Transport(connection)
                transport.add_server_key(host_key)
                transport.start_server(server=server)
                channel = transport.accept(20)
                if mode in ("reject", "bad-password"):
                    return
                assert channel is not None, "No SSH session channel"
                channel.settimeout(20)
                assert server.shell.wait(20), "No shell request"
                assert server.resized.wait(20), "No window-size request"
                assert server.dimensions == (53, 24), server.dimensions
                command = b""
                while len(command) < 17:
                    part = channel.recv(17 - len(command))
                    assert part, "Client closed before sending input"
                    command += part
                assert command == b"integration-ping\n", command
                payload = bytes(ord("a") + i % 26 for i in range(65536))
                channel.sendall(payload)
                # The client closes only after validating every byte.
                while channel.recv(128):
                    pass
            except (EOFError, paramiko.SSHException, ConnectionResetError) as error:
                if mode not in ("reject", "bad-password") and not server.resized.is_set():
                    failures.append(error)
            except Exception as error:
                failures.append(error)
            finally:
                if transport:
                    transport.close()

        worker = threading.Thread(target=serve)
        worker.start()
        password = "" if mode.startswith(("rsa", "ec")) else "wrong" if mode == "bad-password" else "correct"
        result = subprocess.run(
            [str(executable), str(port), password,
             str(key_path) if mode.startswith(("rsa", "ec")) or mode == "fallback" else "", mode],
            capture_output=True, text=True, timeout=35,
        )
        worker.join(timeout=35)
        assert not worker.is_alive(), "SSH server did not stop"
        assert result.returncode == 0, f"{mode}: {result.stdout}\n{result.stderr}"
        assert not failures, f"{mode}: {failures}"
        if mode == "reject":
            assert server.auth_calls == 0, "Credentials sent to rejected host!"
        if mode == "bad-password":
            assert server.auth_calls == 1, "The same bad password was retried!"
        print(f"{mode}: {result.stdout.strip()}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=pathlib.Path)
    args = parser.parse_args()
    host_key = paramiko.RSAKey.generate(2048)
    with tempfile.TemporaryDirectory(prefix="picocalc-ssh-test-") as directory:
        root = pathlib.Path(directory)
        keys = {}
        for name, private in (
            ("rsa", rsa.generate_private_key(public_exponent=65537, key_size=2048)),
            ("ec", ec.generate_private_key(ec.SECP256R1())),
        ):
            path = root / f"{name}.pem"
            path.write_bytes(private.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            ))
            key_class = paramiko.RSAKey if name == "rsa" else paramiko.ECDSAKey
            keys[name] = (path, key_class.from_private_key_file(str(path)))
            for suffix, encoding in (("der", serialization.Encoding.DER), ("pkcs8", serialization.Encoding.PEM)):
                other = root / f"{name}-{suffix}.key"
                other.write_bytes(private.private_bytes(
                    encoding, serialization.PrivateFormat.PKCS8, serialization.NoEncryption(),
                ))
                keys[f"{name}-{suffix}"] = (other, keys[name][1])
        for mode in ("password", "rsa", "ec", "rsa-der", "ec-der", "rsa-pkcs8",
                     "ec-pkcs8", "fallback", "bad-password", "reject"):
            path, key = keys.get(mode, keys["rsa"])
            run_case(args.executable.resolve(), mode, path, key, host_key)
        run_case(args.executable.resolve(), "password-ec-host", *keys["rsa"], paramiko.ECDSAKey.generate())
        ed_path = root / "host_ed25519"
        from cryptography.hazmat.primitives.asymmetric import ed25519
        ed_path.write_bytes(ed25519.Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH, serialization.NoEncryption(),
        ))
        run_case(args.executable.resolve(), "password-ed-host", *keys["rsa"],
                 paramiko.Ed25519Key.from_private_key_file(str(ed_path)))


if __name__ == "__main__":
    main()
