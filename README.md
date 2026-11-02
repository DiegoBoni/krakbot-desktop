# krakbot Desktop

Firmware para la **Waveshare ESP32-S3-Touch-LCD-1.54**: un gadget de escritorio a batería con
tres "apps" que cambian según cómo lo apoyás o lo girás (acelerómetro QMI8658). Estética 8-bit.

| Posición | App |
|---|---|
| Vertical (botones arriba) | **Pomodoro**: foco / descanso / pausa larga, táctil + botones + sonidos |
| Giro 90° a la izquierda | **Reloj**: hora (NTP), clima exterior (Open-Meteo, Rosario), batería, campanita por hora |
| Giro 90° a la derecha | Agente de voz (en desarrollo) |
| Acostada, pantalla arriba | Pantalla apagada (ahorro de batería) |

## Botones
- **PLUS (GPIO4)**: Pomodoro, iniciar/pausar (mantener: reset). Reloj, campanita por hora.
- **BOOT (GPIO0)**: Pomodoro, cambiar modo / saltar bloque. Reloj, ciclo de brillo.
- **PWR (GPIO5)**: reservado para encender/apagar.

## Compilar y subir
Requiere [PlatformIO](https://platformio.org/).

```sh
cp src/secrets.example.h src/secrets.h   # completar WIFI_SSID y WIFI_PASS
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

## Notas de hardware
- LCD ST7789 240x240 por SPI; en rotaciones 2/3 necesita offset de 80 px.
- IMU, táctil CST816 y codec ES8311 comparten el bus I2C (SDA 42, SCL 41).
- GPIO2 debe estar en alto para mantener la alimentación a batería; la tensión de batería se lee en GPIO1 (divisor 1:3).
- `src/es8311.*` viene de los ejemplos de Waveshare.
