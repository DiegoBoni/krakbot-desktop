<h1 align="center">krakbot Desktop</h1>

<p align="center">
  <img src="assets/screens/hero.png" alt="Las cuatro pantallas de krakbot Desktop" width="100%">
</p>

<p align="center">
  Un gadget de escritorio a batería con estética 8-bit: <b>girás la placa y cambia de app</b>.<br>
  Hecho para la <b>Waveshare ESP32-S3-Touch-LCD-1.54</b> (acelerómetro QMI8658, micrófono y parlante).
</p>

> Las capturas de este README son maquetas dibujadas con las mismas medidas que el firmware, no fotos de la placa.

## Cómo funciona

El acelerómetro detecta cómo está la placa y elige la app:

| Posición | App |
|---|---|
| Vertical (botones arriba) | **Pomodoro**: foco / descanso / pausa larga, táctil + botones + sonidos |
| Giro 90° a la izquierda | **Reloj**: hora (NTP), clima exterior (Open-Meteo, Rosario), batería, campanita por hora |
| Giro 90° a la derecha | **Agente de voz**: hablás y un personaje te contesta (Kraken o Frida) |
| Acostada, pantalla arriba | Pantalla apagada (ahorro de batería) |

## Las apps

<table>
  <tr>
    <td align="center"><img src="assets/screens/pomodoro.png" width="260"><br><b>Pomodoro</b><br>25 / 5 / 15 min, 4 bloques por set</td>
    <td align="center"><img src="assets/screens/reloj.png" width="260"><br><b>Reloj + clima</b><br>hora, fecha, temperatura y humedad exterior</td>
  </tr>
  <tr>
    <td align="center"><img src="assets/screens/kraken_habla.png" width="260"><br><b>Kraken</b><br>pulpo pixelado, voz de nene robótico</td>
    <td align="center"><img src="assets/screens/frida_habla.png" width="260"><br><b>Frida</b><br>pícara y coqueta, voz joven</td>
  </tr>
</table>

## Los personajes

<p align="center">
  <img src="assets/screens/personajes.png" alt="Kraken y Frida en sus distintos estados" width="100%">
</p>

Cada uno tiene su propia voz, su personalidad (su propio flujo de n8n) y se cambia manteniendo `BOOT`.
Se animan en reposo, escuchando, pensando, hablando, error y dormido.

<p align="center">
  <img src="assets/screens/frida_contenta.png" width="220" alt="Frida contenta al tocarla">
</p>

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

Dos skins, con aspecto y voz propios (se cambia manteniendo `BOOT`):

- **Kraken**: pulpo pixelado, voz de nene robótico y juguetón (`echo` + efecto metálico).
- **Frida**: retrato pixel art de una chica de pelo rosa y azul, pícara y coqueta: parpadea, guiña de vez en cuando, mueve la boca al hablar, se ríe al tocarla y se duerme. Voz joven y juguetona (`nova`). Los frames salen del atlas `assets/frida_atlas_v2.webp` (3×3: ojos abiertos / cerrados / guiño × boca cerrada / chica / risa) con `python3 tools/make_avatar.py`, que genera `src/avatar_data.h` y `assets/frida_atlas.png`.

Ambos se animan en reposo, escuchando, pensando, hablando, error y dormido.
Mantené apretado `+` y hablá; al soltar:

1. Groq (`whisper-large-v3-turbo`) transcribe el audio.
2. El texto va por POST `{"message": "..."}` al webhook de n8n, que responde `{"text": "..."}`.
3. OpenAI TTS (`gpt-4o-mini-tts`) lo dice en voz alta mientras el personaje mueve la boca.

| Control | Acción |
|---|---|
| Mantener `+` | Grabar (hasta 15 s); soltar para enviar |
| Tocar `+` mientras habla | Interrumpe la respuesta |
| `BOOT` en reposo | Historial: última charla; cada toque va a una más vieja |
| `BOOT` ocupado | Cancela el turno actual |
| Mantener `BOOT` (0,7 s) | Cambia de skin (aspecto + voz); se recuerda al reiniciar |
| Tocar la pantalla | Despierta del modo ZZZ (a los 45 s sin uso) y el personaje reacciona; sale del historial |

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
| `N8N_WEBHOOK_URL` | URL de producción del webhook de n8n de Kraken | ver el paso siguiente |
| `N8N_WEBHOOK_URL_FRIDA` | (opcional) webhook de n8n propio de Frida; si falta usa el de Kraken | ver el paso siguiente |

Después compilá y subí con `pio run -t upload`. Si falta alguna clave, la pantalla del agente dice
"FALTAN CLAVES EN SECRETS.H".

#### 2. Flujos de n8n (uno por personaje)
Cada skin habla con su propio webhook, así cada personaje tiene su personalidad:

| Skin | Flujo | Path del webhook |
|---|---|---|
| Kraken | [`n8n/krakbot-workflow.json`](n8n/krakbot-workflow.json) | `/webhook/krakbot` |
| Frida | [`n8n/frida-workflow.json`](n8n/frida-workflow.json) | `/webhook/frida` |

Los JSON no traen credenciales. Para cada uno:

1. En n8n: **Workflows → Import from file** y elegí el JSON.
2. Asigná tus credenciales en los nodos que las piden:
   - **OpenAI Chat Model**: tu credencial *OpenAI API*.
   - **MCP Client** (opcional, herramientas de Composio): una credencial *Header Auth* con tu clave de
     Composio. Si no lo querés, borrá el nodo.
3. Activá el workflow (**Active**) y copiá la **Production URL** del nodo Webhook
   (`https://TU-N8N/webhook/krakbot` o `/webhook/frida`, no la de `webhook-test`) en
   `N8N_WEBHOOK_URL` o `N8N_WEBHOOK_URL_FRIDA`.

Cada flujo: Webhook (`POST`, recibe `{"message": "..."}`) → AI Agent (GPT-5.1, memoria de ventana
y personalidad) → Respond to Webhook (`{"text": "..."}`). La personalidad se cambia en el
*System Message* del nodo AI Agent de cada flujo. Para sumar otro personaje, duplicá un flujo,
cambiá el path del webhook y su prompt.

#### 3. Voz
Los skins (aspecto y voz) se definen en la tabla `SKINS` de `src/agent.cpp`: voz de OpenAI, instrucciones de estilo y cuánto efecto robótico lleva.
Las conexiones HTTPS validan certificados con el bundle de CA del core, por lo que necesitan la hora NTP.

## Imágenes del README
Se generan con `PRESS_START_2P=/ruta/PressStart2P-Regular.ttf python3 tools/render_readme_images.py`
(fuente [Press Start 2P](https://fonts.google.com/specimen/Press+Start+2P), licencia OFL).
