#!/usr/bin/env python3
"""Optional cross-platform receiver; Windows users can use receive-logs.bat without Python."""
import argparse, datetime, pathlib, socket

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=45879)
    parser.add_argument('--folder', default=str(pathlib.Path(__file__).parent / 'logs'))
    args = parser.parse_args()
    folder = pathlib.Path(args.folder)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / ('altek-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.log')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, path.open('a', encoding='utf-8') as log:
        sock.bind(('0.0.0.0', args.port))
        print(f'Listening UDP {args.port}; saving {path}', flush=True)
        try:
            while True:
                data, remote = sock.recvfrom(65535)
                msg = data.decode('utf-8', errors='replace')
                msg = ''.join(c if c >= ' ' and c != '\x7f' else '?' for c in msg)
                line = f'{datetime.datetime.now():%H:%M:%S.%f} {remote[0]} {msg}'
                print(line, flush=True)
                log.write(line + '\n')
                log.flush()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
