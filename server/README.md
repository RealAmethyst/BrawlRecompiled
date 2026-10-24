# Brawl Recompiled online server

One small Python program (no dependencies, Python 3.8+) that every copy of Brawl Recompiled can reach: it keeps the
list of public rooms and relays the games' packets between players, so online play works behind any router with no
port forwarding. It uses one UDP port, 52200 by default, and very little bandwidth (a match is a few kilobytes a
second per player).

```bash
python3 brawl_online_server.py --port 52200
```

The games find the server through `[online] server` in `Config.toml` (the launcher's Online page sets it), or the
address built into the game when there is one. A room code is the host's id on the server, so codes only work between
players on the same server.

## Running it for everyone

Any always-on machine with a public IP and UDP port 52200 open works. Free or nearly free options:

### Oracle Cloud Always Free (free)

1. Create an account at <https://www.oracle.com/cloud/free/> and make a VM with the "Always Free" shape, Ubuntu image.
2. In the VM's subnet, open **Security List > Add Ingress Rule**: source `0.0.0.0/0`, protocol **UDP**, port **52200**.
3. On the VM:
   ```bash
   sudo iptables -I INPUT -p udp --dport 52200 -j ACCEPT
   sudo netfilter-persistent save
   sudo mkdir -p /opt/brawl && sudo cp brawl_online_server.py /opt/brawl/
   sudo cp brawl-online.service /etc/systemd/system/
   sudo systemctl enable --now brawl-online
   journalctl -u brawl-online -f      # see it running
   ```
4. The VM's public IP (or a domain name pointed at it) plus `:52200` is the server address.

### Fly.io (about $2 a month: UDP needs a dedicated IPv4 there)

```bash
fly launch --no-deploy --copy-config --name brawl-online   # in this folder, with fly.toml and the Dockerfile
fly ips allocate-v4
fly deploy
```
The address is `brawl-online.fly.dev:52200`.

### Your own PC

Run the server, forward UDP port 52200 on your router to that PC, and give players your public IP (or a dynamic-DNS
name). It must stay running while people play.

## Making it the built-in server

Put the address in `kDefaultOnlineServer` in `wiicompiled/runtime/src/netplay/net_socket.cpp` and rebuild, so players
don't have to set anything.
