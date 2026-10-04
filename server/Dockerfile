FROM python:3.11-slim

# Install ffmpeg and other system dependencies
RUN apt-get update && apt-get install -y \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install -r requirements.txt

COPY proxy_server_audio.py .
COPY static/ static/

# Create tmp directory that the app uses
RUN mkdir -p /tmp

EXPOSE 8080
CMD exec gunicorn --bind :$PORT --workers 1 --timeout 60 proxy_server_audio:app