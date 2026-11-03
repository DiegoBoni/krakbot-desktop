// Copy to secrets.h (git-ignored) and fill in your 2.4 GHz Wi-Fi.
#pragma once
#define WIFI_SSID ""
#define WIFI_PASS ""

// Voice agent (right pose).
#define GROQ_API_KEY ""                   // https://console.groq.com/keys
#define OPENAI_API_KEY ""                 // https://platform.openai.com/api-keys
#define N8N_WEBHOOK_URL ""                // production URL of the n8n webhook (POST {"message": "..."} -> {"text": "..."})
#define N8N_WEBHOOK_URL_FRIDA ""               // optional: separate n8n flow for Frida (falls back to N8N_WEBHOOK_URL)
