# Pendientes / ideas futuras

## Temporizador local en el ESP32

Objetivo: poder poner un temporizador en el propio MiniSecIA sin depender de que el servidor lo dispare.

### Viabilidad

Sí puede correr en local. El firmware ya usa despertar por timer (rotación de imagen) y RTC. Un temporizador típico sería:

1. Guardar el instante de aviso (NVS y/o RTC).
2. Dormir en deep sleep hasta esa hora (o el mínimo entre imagen y temporizador).
3. Al despertar: tono (y opcionalmente cambio de imagen / mensaje en e-Paper).

### Ideas de UX (por decidir)

- **Por voz** («temporizador diez minutos»): lo más cómodo, pero el NLU conviene hacerlo en ConvocAUTOrias y devolver la duración al dispositivo (o un comando ya parseado). En local puro haría falta un parser muy limitado.
- **Por botones**: p. ej. pulsar varias veces / menú corto para 1 / 5 / 10 / 30 min, sin red.
- **Híbrido**: grabas la petición; si hay red se confirma al momento; si no, queda en cola y el timer no arranca hasta sync (peor para cocina).

### Detalles técnicos a resolver

- Varios temporizadores a la vez vs uno solo.
- Cancelar el temporizador activo (PWR / BOOT).
- Batería: despertar solo para el aviso, no mantener Wi‑Fi.
- Conflicto con rotación de imagen y con grabación/escucha en curso.
- Persistencia tras apagado total (PWR largo corta batería: el timer no puede sobrevivir sin alimentación).

### Estado

Aplazado. No implementar hasta definir UX (voz vs botones vs híbrido).
