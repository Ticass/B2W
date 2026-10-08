FROM python:3.12-slim
WORKDIR /app
COPY services/discord_support/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r requirements.txt \
    && useradd --uid 10001 --create-home support \
    && mkdir /data && chown support:support /data
COPY services/discord_support/core.py services/discord_support/bot.py /app/
USER support
ENV PYTHONUNBUFFERED=1 STATE_PATH=/data/support.sqlite3
CMD ["python", "bot.py"]
