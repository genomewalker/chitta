FROM python:3.12 AS build
COPY requirements.txt /app/requirements.txt
RUN pip install -r /app/requirements.txt
FROM alpine:3.20 AS runtime
COPY --from=0 /app /app
COPY config.yaml /app/config.yaml
