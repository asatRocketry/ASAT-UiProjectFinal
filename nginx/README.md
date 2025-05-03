## Certificate Creation
Adjust the certified IP addresses in openssl-san.cnf and then run this command:

<code>
openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout privkey.pem -out cert.pem \
  -config openssl-san.cnf
<code>

## Load the nginx config and start the service
Run this in the nginx folder:
`sudo nginx -c $(pwd)/nginx.conf -p $(pwd)`