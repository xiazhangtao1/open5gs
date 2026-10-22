FROM python:3.12-slim
WORKDIR /opt/xcn-monitor
COPY monitor/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY monitor/ .
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
USER 65532:65532
ENTRYPOINT ["python3", "server.py"]
