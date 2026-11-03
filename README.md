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
Las claves van en `src/secrets.h` (ignorado por git): `GROQ_API_KEY`, `OPENAI_API_KEY` y
`N8N_WEBHOOK_URL`. El flujo de n8n es un Webhook (`POST /krakbot`) → AI Agent con memoria →
Respond to Webhook. La voz y el estilo se ajustan con `TTS_VOICE` y `TTS_STYLE` en `src/agent.cpp`.
Las conexiones HTTPS validan certificados con el bundle de CA del core, por lo que necesitan la hora NTP.
