FROM ghcr.io/berriai/litellm@sha256:f63fb81b831b170ec16851e23c36ac5bf52ef106b271406429524a2ed730bbfd
COPY patch_models.py /app/patch_models.py
RUN /app/.venv/bin/python /app/patch_models.py
