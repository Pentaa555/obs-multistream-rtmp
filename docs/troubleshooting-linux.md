# Solución de problemas (Linux / Flatpak)

Guía para poner en marcha el login de Facebook del plugin en un equipo nuevo con
OBS Studio instalado como Flatpak.

## Requisitos previos (una sola vez por equipo)

### 1. Permitir acceso al llavero (persistencia de sesión)

El plugin guarda el token de Facebook en el llavero del sistema mediante
libsecret (Secret Service). El sandbox de Flatpak de OBS **no** concede acceso a
`org.freedesktop.secrets` por defecto, y una extensión Flatpak no puede
concederse ese permiso a sí misma. Sin esto, el plugin pide reconectar en cada
arranque.

Concede el permiso una vez:

```bash
flatpak override --user --talk-name=org.freedesktop.secrets com.obsproject.Studio
```

Reinicia OBS después de aplicarlo. Para revertir:

```bash
flatpak override --user --nofilesystem=... # no aplica
flatpak override --user --reset com.obsproject.Studio   # borra TODOS los overrides
```

(o edita `~/.local/share/flatpak/overrides/com.obsproject.Studio`).

### 2. IPv6 roto: el login se queda "Sin conexión" y da timeout

**Síntoma:** el login abre el navegador, capturas el token correctamente, pero
la llamada a la Graph API (`/me`) se queda colgada ~15 s y termina con
"A network error occurred while contacting Facebook: ... Operation timed out".
El dock muestra "Sin conexión".

**Causa raíz:** en algunas redes, los servidores de Facebook
(`graph.facebook.com`) resuelven direcciones IPv6 que **no son alcanzables**
(IPv6 mal configurado en el router/ISP). El motor de red de Qt
(`QNetworkAccessManager`) intenta la dirección IPv6 primero y no hace un
fallback rápido a IPv4 (no implementa "Happy Eyeballs" como curl), así que se
cuelga hasta agotar el tiempo. Con `curl` funciona porque prueba IPv4 e IPv6 a
la vez; con Qt no.

**Cómo confirmarlo** (desde una terminal del host):

```bash
# IPv6 falla (timeout), IPv4 responde rápido (400 sin token es lo esperado):
flatpak run --command=sh com.obsproject.Studio -c \
  'curl -6 -s -o /dev/null -w "IPv6: %{http_code} %{time_total}s\n" --max-time 12 https://graph.facebook.com/v26.0/me;
   curl -4 -s -o /dev/null -w "IPv4: %{http_code} %{time_total}s\n" --max-time 12 https://graph.facebook.com/v26.0/me'
```

Si `IPv6` da `000` (timeout) y `IPv4` da `400` rápido, es este problema.

**Solución A — Deshabilitar IPv6 en el sistema (recomendada, simple):**

Temporal (se revierte al reiniciar el equipo):

```bash
sudo sysctl -w net.ipv6.conf.all.disable_ipv6=1 net.ipv6.conf.default.disable_ipv6=1
```

Permanente:

```bash
echo -e "net.ipv6.conf.all.disable_ipv6 = 1\nnet.ipv6.conf.default.disable_ipv6 = 1" | \
  sudo tee /etc/sysctl.d/99-disable-ipv6.conf
sudo sysctl --system
```

Para revertir: borra `/etc/sysctl.d/99-disable-ipv6.conf` y ejecuta
`sudo sysctl -w net.ipv6.conf.all.disable_ipv6=0 net.ipv6.conf.default.disable_ipv6=0`,
o reinicia.

**Solución B — Arreglar la conectividad IPv6** en el router/ISP (si debería
funcionar). Es lo correcto a largo plazo pero depende de tu red.

> Nota: deshabilitar IPv6 afecta a todo el sistema, no solo a OBS. Si usas otros
> servicios que dependen de IPv6, prefiere la solución B.

## Requisitos en el panel de Meta (una sola vez por app)

La app Meta usada por el plugin es `1609511030686437`. En
developers.facebook.com → la app → **Inicio de sesión con Facebook →
Configuración**, registra como **URI de redireccionamiento de OAuth válido**:

```
https://pentamultistream.online/oauth.html
```

Mientras la app esté en modo Desarrollo, solo funcionará con cuentas que tengan
rol (administrador/desarrollador/probador) en la app.

## Cómo funciona el login (referencia)

1. El plugin abre un servidor loopback local en `127.0.0.1:<puerto>` y abre el
   navegador del sistema en el diálogo OAuth de Facebook, pasando el puerto
   dentro del parámetro `state` (`<aleatorio>.<puerto>`).
2. Facebook redirige a la página puente HTTPS
   (`https://pentamultistream.online/oauth.html`) con el token en el fragmento
   (o un error en el query string si se cancela/niega).
3. La página puente reenvía el resultado, mediante una navegación de nivel
   superior a `http://127.0.0.1:<puerto>/`, al servidor loopback del plugin.
4. El plugin valida el `state`, obtiene la cuenta y las Páginas vía Graph API y
   guarda el token en el llavero.

Así el login no requiere copiar/pegar y detecta automáticamente cuando el
usuario niega los permisos.

## Verificación rápida

Tras aplicar los requisitos previos y reiniciar OBS:

1. Panel **Multistream RTMP** → **Conectar Facebook**.
2. Autoriza en el navegador.
3. El dock debe mostrar tu cuenta conectada.
4. Cierra y reabre OBS: la sesión debe restaurarse sin pedir reconectar.
