FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY src/ ./src/

# `python -m slotsync` resolves the package from here.
ENV PYTHONPATH=/app/src

ENV SLOTSYNC_DATA=/data
VOLUME ["/data"]

EXPOSE 8080/tcp 9977/udp

# Reads the port from the environment rather than hardcoding 8080, so changing
# SLOTSYNC_HTTP_PORT does not silently mark a healthy container unhealthy.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD python -c "import os,urllib.request,sys; p=os.environ.get('SLOTSYNC_HTTP_PORT','8080'); sys.exit(0 if urllib.request.urlopen(f'http://127.0.0.1:{p}/healthz').status==200 else 1)"

CMD ["python", "-m", "slotsync"]
