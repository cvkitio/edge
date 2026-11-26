FROM python:3.12-slim
WORKDIR /app
COPY pyproject.toml ./
COPY cvkit_verify cvkit_verify
COPY evals/__init__.py evals/run.py evals/publish.py evals/monthly.py evals/models.json evals/
RUN pip install --no-cache-dir . && useradd -u 1000 -m cvkit
USER 1000
ENV PYTHONUNBUFFERED=1 PYTHONFAULTHANDLER=1
CMD ["cvkit-verify", "-c", "/etc/cvkit-verify/config.toml"]
