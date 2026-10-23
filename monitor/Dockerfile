FROM python:3.12-slim AS native
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev && rm -rf /var/lib/apt/lists/*
COPY lib/proto/ue-trace.c lib/proto/ue-trace.h /src/
RUN cc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared /src/ue-trace.c -o /libxcn-trace.so

FROM python:3.12-slim
WORKDIR /opt/xcn-monitor
COPY monitor/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY monitor/ .
COPY --from=native /libxcn-trace.so /opt/xcn-monitor/libxcn-trace.so
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
USER 65532:65532
ENTRYPOINT ["python3", "server.py"]
