import datetime
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
import ipaddress

probe = sys.argv[1]
now = datetime.datetime.now(datetime.timezone.utc)


def certificate(key, issuer, issuer_key, ip, expired=False):
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'TLS test leaf' if issuer else 'TLS test CA')])
    return (x509.CertificateBuilder().subject_name(name).issuer_name(issuer.subject if issuer else name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=2))
            .not_valid_after(now + datetime.timedelta(days=-1 if expired else 1))
            .add_extension(x509.BasicConstraints(ca=issuer is None, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(ip))]), critical=False)
            .sign(issuer_key or key, hashes.SHA256()))


with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca = certificate(ca_key, None, None, '127.0.0.1')
    ca_path = root / 'ca.crt'
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    for label, ip, expired, trust, expected in [
        ('trusted IP', '127.0.0.1', False, str(ca_path), True),
        ('wrong IP', '127.0.0.2', False, str(ca_path), False),
        ('expired certificate', '127.0.0.1', True, str(ca_path), False),
        ('untrusted CA', '127.0.0.1', False, '', False),
        ('missing provisioned CA', '127.0.0.1', False, str(root / 'missing'), False),
    ]:
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        cert = certificate(key, ca, ca_key, ip, expired)
        cert_path, key_path = root / 'server.crt', root / 'server.key'
        cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert_path, key_path)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(5)
            def serve():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        with context.wrap_socket(connection, server_side=True) as tls:
                            if tls.recv(1024):
                                tls.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecure')
                except (ssl.SSLError, OSError):
                    pass
            worker = threading.Thread(target=serve)
            worker.start()
            result = subprocess.run([probe, '127.0.0.1', str(listener.getsockname()[1]), trust, f'https://127.0.0.1:{listener.getsockname()[1]}/'], capture_output=True, text=True, timeout=10)
            worker.join(6)
            assert not worker.is_alive(), label
            assert (result.returncode == 0) == expected, (label, result.stderr)
            print('PASS', label)
print('ALL PASS')
