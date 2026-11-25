# Changelog

Cambios relevantes del firmware MiniSecIA (Waveshare ESP32-S3-ePaper-1.54G).
El formato sigue la idea de [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).
Las fechas son las del trabajo en el dispositivo (hora local Europe/Madrid).

## [Sin publicar] — 2026-11-25

### Añadido

- **Meditación**: despertar por temporizador con tono propio (inicio y fin de práctica).
  Config vía `GET /meditacion/config` en cada sync (PWR corto); se programa desde Telegram
  (`/meditacion`). Respeta ventana horaria y pausa.

---

## [Sin publicar] — 2026-11-24

### Añadido

- Respuesta hablada efímera tras `POST /voz`: si llegan `X-Voz-Audio` y `X-Voz-Audio-Path`,
  se descarga `/voz/respuesta/{id}.opus`, se reproduce, se borra en la SD y se hace ACK.
  No usa la cola `/escucha` (temperatura / previsión).

---

## [Sin publicar] — 2026-11-23

### Cambiado

- Flag NVS `subida_ok`: tras grabar solo se intenta subir si la última subida fue correcta.
- Sin red / `SUBIDA_REINTENTAR` → `subida_ok=0` y no se reintenta en cada grabación; **PWR corto** vuelve a intentarlo y, si va bien, restaura el flag.
- Ideas de **temporizador local** anotadas en `pendientes.md` (sin implementar).

### Documentación

- README: comportamiento condicional de subida tras grabar.
- `pendientes.md`: temporizador en el ESP32.

---

## [Sin publicar] — 2026-11-18

### Cambiado

- La imagen decorativa rota cada **4 h** (`imagen_horas`, default en Kconfig/`config.txt.ejemplo`).
- **PWR corto** sincroniza cola, escucha e (si toca) manifiesto de imágenes, **sin** cambiar la foto de pantalla.
- La rotación queda solo a cargo del temporizador local (`imagenes_rotar_si_toca` / despertar por timer).

### Documentación

- README y `config.txt.ejemplo` alineados con el nuevo comportamiento de pantalla y sync.

---

## [Sin publicar] — 2026-11-17

### Añadido

- **Escucha**: descarga de Ogg Opus (`GET /audios/manifest`, `/audios/{id}.opus`, `POST …/ack`) a `/escucha` en la microSD.
- Reproducción Opus en el ES8311 (`audio_reproducir_opus`) y módulo `escucha.c` / `escucha.h`.
- **BOOT largo**: reproduce la escucha más reciente (también las ya oídas, sin Wi‑Fi).
- Durante la reproducción: **BOOT corto** = anterior, **PWR corto** = siguiente (o para si es la última).
- Carpetas `/escucha` y `/escucha/hechos` en la SD.

### Cambiado

- El Wi‑Fi **ya no se busca solo** en sueño: un intento al terminar de grabar; el resto espera a **PWR corto**.
- **PWR corto** en reposo pasa a ser la sincronización principal (cola + audios de escucha + sync de imágenes si toca).
- **BOOT largo** deja de forzar sync + cambio de imagen; ahora abre la escucha.
- **PWR** al despertar del sueño solo enciende el dispositivo; hay que pulsar **PWR corto** despierto para sincronizar.
- Flag `SYNC_SOLO_SUBIR` para el intento post-grabación (sube cola sin bajar audios ni imágenes).
- Aviso de batería baja al sincronizar o al encender (además del tono).
- Dependencia `esp_audio_codec` para decodificar Opus.

### Documentación

- Manual de uso: sección Escuchar, botones, sonidos, carpetas SD y endpoints de audios.

---

## 2026-11-16 — `7ca8ad7`

### Cambiado

- Renombre del proyecto de Minisecretario a **MiniSecIA** (CMake, Kconfig, README, boundary multipart).

---

## 2026-11-16 — `5d383b7`

### Añadido

- Firmware inicial ESP-IDF para Waveshare ESP32-S3-ePaper-1.54G.
- Grabación de notas de voz (WAV 16 kHz) y subida a ConvocAUTOrias (`POST /voz`).
- Cola en microSD (`/cola`, `/fallidos`), tonos ES8311, LED y botones BOOT/PWR (corto/largo).
- Sueño profundo con despertar por BOOT, PWR o temporizador; RTC y SNTP.
- Imágenes decorativas e-Paper (4 colores): rotación local (`imagen_horas`, default 6 h) y sync desde servidor (`/imagenes/manifest`).
- Ajustes por `config.txt` / NVS / `menuconfig` (Wi‑Fi multi-red, token, volumen, ganancia, timeouts).
- README de puesta en marcha, mapa de pines y script de conversión de imágenes.
