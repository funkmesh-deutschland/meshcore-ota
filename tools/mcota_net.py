import socket
import ssl
import urllib.error
import urllib.request

AGENT = "meshcore-ota"


def context():
    ctx = ssl.create_default_context()
    try:
        import certifi
        ctx.load_verify_locations(certifi.where())
    except Exception:
        pass
    return ctx


def open_url(url, timeout, headers=None):
    request = urllib.request.Request(url, headers=dict({"User-Agent": AGENT}, **(headers or {})))
    return urllib.request.urlopen(request, timeout=timeout, context=context())


def fetch(url, timeout, headers=None):
    with open_url(url, timeout, headers) as response:
        return response.read()


def reason(error):
    if isinstance(error, urllib.error.HTTPError):
        if error.code in (403, 429):
            return f"GitHub refused the request (HTTP {error.code}), probably too many requests from this address. Try again later"
        return f"the server answered with HTTP {error.code}"
    cause = getattr(error, "reason", error)
    if isinstance(cause, ssl.SSLError):
        return f"the certificate of the server could not be verified ({cause})"
    if isinstance(cause, (socket.timeout, TimeoutError)):
        return "the server did not answer in time"
    return f"no connection ({cause})"
