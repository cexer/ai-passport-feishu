# Receive only bounded synthetic patterns; never proxies traffic or echoes user data.
from pathlib import Path
import argparse,socketserver,ssl,threading,time,json,struct
parser=argparse.ArgumentParser(description="Bounded synthetic TCP/TLS pattern receiver, no IM or user data")
parser.add_argument('--config',default='build/network-probe/config.json')
parser.add_argument('--timeout',type=int,default=600)
args=parser.parse_args()
if not 1<=args.timeout<=3600:parser.error('Timeout must be 1..3600 seconds')
config_path=Path(args.config);p=config_path.parent
config=json.loads(config_path.read_text(encoding='utf-8'))
host=config['host'];tcp_port=int(config['tcp_port']);tls_port=int(config['tls_port'])
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.minimum_version=ssl.TLSVersion.TLSv1_2
context.maximum_version=ssl.TLSVersion.TLSv1_2
context.load_cert_chain(p/'server-cert.pem',p/'server-key.pem')
lock=threading.Lock()
def log(data):
    with lock:
        with (p/'server-results.jsonl').open('a',encoding='utf-8') as f:f.write(json.dumps(data)+'\n')
        print(json.dumps(data),flush=True)
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        tls=self.server.secure
        sock=self.request;start=time.monotonic()
        try:
            sock.settimeout(8)
            if tls:sock=context.wrap_socket(sock,server_side=True)
            def read(n):
                out=b''
                while len(out)<n:
                    data=sock.recv(n-len(out))
                    if not data:raise EOFError()
                    out+=data
                return out
            n=struct.unpack('!I',read(4))[0]
            if n>32768:raise ValueError('size')
            data=read(n)
            valid=data==bytes(i%256 for i in range(n))
            sock.sendall((b'OKAY' if valid else b'FAIL')+struct.pack('!I',n))
            log({'tls':tls,'bytes':n,'pattern_valid':valid,'ms':round((time.monotonic()-start)*1000)})
        except Exception as e:log({'tls':tls,'error':type(e).__name__,'ms':round((time.monotonic()-start)*1000)})
        finally:
            try:sock.close()
            except OSError:pass
servers=[]
try:
    for port in (tcp_port,tls_port):
        s=Server((host,port),Handler);s.secure=port==tls_port;servers.append(s)
        threading.Thread(target=s.serve_forever,daemon=True).start()
    log({'ready':True,'ports':[tcp_port,tls_port]})
    start=time.monotonic()
    while time.monotonic()-start<args.timeout and not (p/'server.stop').exists():time.sleep(.5)
finally:
    for s in servers:s.shutdown();s.server_close()
    log({'stopped':True})
