# krakbot Desktop

Firmware para la **Waveshare ESP32-S3-Touch-LCD-1.54**: un gadget de escritorio a batería con
tres "apps" que cambian según cómo lo apoyás o lo girás (acelerómetro QMI8658). Estética 8-bit.

| Posición | App |
|---|---|
| Vertical (botones arriba) | **Pomodoro**: foco / descanso / pausa larga, táctil + botones + sonidos |
| Giro 90° a la izquierda | **Reloj**: hora (NTP), clima exterior (Open-Meteo, Rosario), batería, campanita por hora |
| Giro 90° a la derecha | Agente de voz: Krakbot, el pulpo pixelado |
| Acostada, pantalla arriba | Pantalla apagada (ahorro de batería) |

## Botones
- **PLUS (GPIO4)**: Pomodoro, iniciar/pausar (mantener: reset). Reloj, campanita por hora.
- **BOOT (GPIO0)**: Pomodoro, cambiar modo / saltar bloque. Reloj, ciclo de brillo.
- **PLUS / BOOT en la pose derecha**: ver *Agente de voz* más abajo.
- **PWR (GPIO5)**: reservado para encender/apagar.

## Compilar y subir
Requiere [PlatformIO](https://platformio.org/).

```sh
cp src/secrets.example.h src/secrets.h   # completar Wi-Fi, GROQ_API_KEY, OPENAI_API_KEY y N8N_WEBHOOK_URL
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

## Notas de hardware
- LCD ST7789 240x240 por SPI; en rotaciones 2/3 necesita offset de 80 px.
- IMU, táctil CST816 y codec ES8311 comparten el bus I2C (SDA 42, SCL 41).
- GPIO2 debe estar en alto para mantener la alimentación a batería; la tensión de batería se lee en GPIO1 (divisor 1:3).
- `src/es8311.*` y `src/es7210.*` (micrófono) vienen de los ejemplos de Waveshare.
- Micrófono ES7210 y parlante ES8311 comparten el bus I2S (MCLK 8, BCLK 9, LRC 10, DIN 11, DOUT 12).

## Agente de voz (pose derecha)

Krakbot, un pulpo pixelado animado (reposo, escuchando, pensando, hablando, error y dormido).
Mantené apretado `+` y hablá; al soltar:

1. Groq (`whisper-large-v3-turbo`) transcribe el audio.
2. El texto va por POST `{"message": "..."}` al webhook de n8n, que responde `{"text": "..."}`.
3. OpenAI TTS (`gpt-4o-mini-tts`) lo dice en voz alta mientras Krakbot mueve la boca.

| Control | Acción |
|---|---|
| Mantener `+` | Grabar (hasta 15 s); soltar para enviar |
| Tocar `+` mientras habla | Interrumpe la respuesta |
| `BOOT` en reposo | Historial: última charla; cada toque va a una más vieja |
| `BOOT` ocupado | Cancela el turno actual |
| Tocar la pantalla | Despierta del modo ZZZ (a los 45 s sin uso) y Krakbot reacciona; sale del historial |

El historial guarda las últimas 8 charlas en memoria (se pierden al reiniciar).
El Wi-Fi se mantiene mientras estés en esta pose y se apaga tras 60 s sin uso.

### Configuración

#### 1. Claves de API
Copiá la plantilla (el archivo real está en `.gitignore` y nunca se sube):

```sh
cp src/secrets.example.h src/secrets.h
```

y completá `src/secrets.h`:

| Define | Qué es | Dónde se obtiene |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASS` | Tu red Wi-Fi de 2,4 GHz | tu router |
| `GROQ_API_KEY` | Transcripción de voz (`gsk_...`) | https://console.groq.com/keys |
| `OPENAI_API_KEY` | Voz de la respuesta (`sk-...`) | https://platform.openai.com/api-keys |
| `N8N_WEBHOOK_URL` | URL de producción del webhook de n8n | ver el paso siguiente |

Después compilá y subí con `pio run -t upload`. Si falta alguna clave, la pantalla del agente dice
"FALTAN CLAVES EN SECRETS.H".

#### 2. Flujo de n8n
El flujo está en [`n8n/krakbot-workflow.json`](n8n/krakbot-workflow.json), sin credenciales:

1. En n8n: **Workflows → Import from file** y elegí ese JSON.
2. Asigná tus credenciales en los nodos que las piden:
   - **OpenAI Chat Model**: tu credencial *OpenAI API*.
   - **MCP Client** (opcional, herramientas de Composio): una credencial *Header Auth* con tu clave de
     Composio. Si no lo querés, borrá el nodo.
3. Activá el workflow (**Active**) y copiá la **Production URL** del nodo Webhook
   (`https://TU-N8N/webhook/krakbot`, no la de `webhook-test`) en `N8N_WEBHOOK_URL`.

Flujo: Webhook (`POST /krakbot`, recibe `{"message": "..."}`) → AI Agent (GPT-5.1, memoria de ventana
y personalidad de Kraken) → Respond to Webhook (`{"text": "..."}`). La personalidad se cambia en el
*System Message* del nodo AI Agent.

#### 3. Voz
La voz y el estilo se ajustan con `TTS_VOICE` y `TTS_STYLE` en `src/agent.cpp`.
Las conexiones HTTPS validan certificados con el bundle de CA del core, por lo que necesitan la hora NTP.
