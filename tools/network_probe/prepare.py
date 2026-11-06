# Generate disposable test certificates, never production credentials.
from pathlib import Path
import argparse,datetime,ipaddress,json
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

parser=argparse.ArgumentParser(description="Create a bounded synthetic LAN probe fixture; no Wi-Fi passwords are needed")
parser.add_argument('--host',required=True,help='LAN IPv4 address of the test server')
parser.add_argument('--ssid',required=True,help='SSID to compare and scan; never provide its password')
parser.add_argument('--output',default='build/network-probe')
parser.add_argument('--tcp-port',type=int,default=19090)
parser.add_argument('--tls-port',type=int,default=19091)
args=parser.parse_args()
host=str(ipaddress.IPv4Address(args.host))
if not 0<len(args.ssid.encode('utf-8'))<=32:parser.error('SSID must contain 1..32 UTF-8 bytes')
if args.tcp_port==args.tls_port or not all(1024<=n<=65535 for n in (args.tcp_port,args.tls_port)):parser.error('Use distinct unprivileged ports')
p=Path(args.output);p.mkdir(parents=True,exist_ok=True)
key=ec.generate_private_key(ec.SECP256R1())
name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,host)])
cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(datetime.datetime(1970,1,1,tzinfo=datetime.timezone.utc))
    .not_valid_after(datetime.datetime(2036,1,1,tzinfo=datetime.timezone.utc))
    .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(host))]),False)
    .add_extension(x509.BasicConstraints(ca=True,path_length=None),True)
    .sign(key,hashes.SHA256()))
pem=cert.public_bytes(serialization.Encoding.PEM)
(p/'server-cert.pem').write_bytes(pem)
(p/'server-key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
(p/'network_probe_config.h').write_text('#pragma once\n'+
    '#define NETWORK_PROBE_HOST '+json.dumps(host)+'\n'+
    '#define NETWORK_PROBE_SSID '+json.dumps(args.ssid)+'\n'+
    f'#define NETWORK_PROBE_TCP_PORT {args.tcp_port}\n#define NETWORK_PROBE_TLS_PORT {args.tls_port}\n'+
    '#define NETWORK_PROBE_CA '+json.dumps(pem.decode('ascii'))+'\n',encoding='utf-8')
(p/'config.json').write_text(json.dumps({'host':host,'tcp_port':args.tcp_port,'tls_port':args.tls_port},indent=2),encoding='utf-8')
stop=p/'server.stop'
if stop.exists():stop.unlink()
print('Synthetic LAN probe configured; keep the generated test key out of commits')
