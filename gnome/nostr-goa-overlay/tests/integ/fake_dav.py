#!/usr/bin/env python3
import http.server
import socketserver
import sys

PORT = 7680

CAL_ICS = """BEGIN:VCALENDAR\nVERSION:2.0\nPRODID:-//Nostr DAV//EN\nBEGIN:VEVENT\nUID:1\nDTSTAMP:20240101T000000Z\nSUMMARY:Test Event\nDTSTART:20240101T010000Z\nDTEND:20240101T020000Z\nEND:VEVENT\nEND:VCALENDAR\n"""

CARD_VCF = """BEGIN:VCARD\nVERSION:3.0\nFN:Test User\nN:User;Test;;;\nEMAIL:test@example.com\nEND:VCARD\n"""

class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/cal/'):
            body = CAL_ICS.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/calendar')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith('/card/'):
            body = CARD_VCF.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/vcard')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

if __name__ == '__main__':
    with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            httpd.server_close()
