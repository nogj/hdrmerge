# Pipeline de HDRMerge experimental

## 1. Qué hace realmente HDRMerge

HDRMerge combina varias capturas RAW de una misma escena en un único DNG RAW.
No revela las fotografías ni mezcla imágenes RGB terminadas. Trabaja con las
muestras lineales del sensor y conserva el mosaico CFA para que el demosaicing,
el balance de blancos, el color y el tone mapping se realicen después en un
revelador externo.

El objetivo tiene dos partes:

1. ampliar el rango dinámico utilizando exposiciones oscuras donde las largas
   están saturadas;
2. reducir ruido combinando observaciones coherentes donde varias exposiciones
   contienen información válida.

Una forma simplificada de describir una muestra de la exposición `k` es:

```text
RAW_k(x,y) = ganancia_k · radiancia_CFA(x,y) + ruido_k
```

Cada coordenada mide solamente uno de los colores del patrón CFA. Por eso todas
las operaciones geométricas deben respetar la fase Bayer y nunca tratar el RAW
como una imagen RGB completa.

## 2. Vista general

```text
Archivos RAW
    |
    v
LibRaw: decodificación y metadatos
    |
    v
Sustracción del negro + comprobación de compatibilidad
    |
    v
Orden de más expuesta a menos expuesta
    |
    +--> estimación del blanco y la saturación segura
    |
    v
Alineamiento MTB entero contra la imagen más oscura
    |
    +--> opcional: refinamiento ECC subpíxel o afín
    |                 |
    |                 v
    |           remuestreo por fase CFA + mapa de validez
    v
Recorte común opcional
    |
    v
Normalización radiométrica global entre exposiciones
    |
    v
Máscara base de exposición segura
    |
    +--> deghosting automático opcional
    +--> edición manual opcional
    |
    v
Feathering espacial guiado radiométricamente
    |
    v
Fusión temporal: Robust / Legacy / Off
    |
    v
Escalado de salida + preview
    |
    v
DNG CFA flotante comprimido + metadatos
```

La GUI y la CLI usan este mismo motor. La diferencia está en cómo reciben las
opciones y en que la GUI permite inspeccionar y editar la máscara.

## 3. Carga y representación interna

### 3.1. Decodificación

`ImageIO::load()` abre cada archivo mediante LibRaw. También admite un único RAW
multifotograma, con un límite de 32 frames. De cada entrada se obtienen:

- dimensiones activas y dimensiones RAW completas;
- márgenes del sensor;
- patrón CFA y número de canales;
- niveles de negro y blanco;
- matriz de color y balance de blancos;
- ISO, tiempo, apertura, orientación y demás metadatos.

Todos los frames deben compartir dimensiones activas, patrón CFA y descripción
de canales. Si una entrada no puede decodificarse o no es compatible, se
descarta la pila completa en lugar de continuar con un estado parcial ambiguo.

### 3.2. Negro y linealidad

Al construir cada `Image`, HDRMerge sustrae el negro correspondiente a la fase
CFA de cada coordenada:

```text
muestra_lineal(x,y) = max(0, RAW(x,y) - negro_CFA(x,y))
```

Desde este punto, cero significa ausencia de señal por encima del negro. El DNG
de salida también declara negro cero, porque la compensación ya está aplicada.

### 3.3. Orden de la pila

Las imágenes se ordenan por brillo medio:

```text
índice 0                 índice N-1
más expuesta  -------->  menos expuesta
```

Esta convención es fundamental:

- la búsqueda de detalle empieza en la captura más luminosa;
- al encontrar saturación se avanza hacia capturas más oscuras;
- la imagen más oscura actúa como referencia geométrica y ancla radiométrica.

El orden no depende exclusivamente de los EXIF. Se deriva de los datos RAW, lo
que evita confiar ciegamente en tiempos o ISO incorrectos.

## 4. Estimación de saturación

HDRMerge necesita distinguir una muestra luminosa válida de una muestra
recortada. El nivel blanco principal procede de los metadatos de LibRaw, llevado
al dominio en el que ya se ha restado el negro.

También se estudia el histograma por fase CFA de la imagen más expuesta. Una
meseta observada sólo refina el blanco si está al menos al 90 % del valor de
metadatos; una escena plana que nunca llega al blanco no se interpreta como
saturación. Salvo que el usuario especifique un blanco manual, se reserva un 1 %
de margen de seguridad.

La saturación se evalúa en un entorno `3x3`, no únicamente en el fotodiodo
central. Esto evita seleccionar como segura una muestra situada junto a un canal
ya recortado.

La confianza de saturación no cambia de uno a cero bruscamente. Entre el 85 % y
el 100 % del umbral utiliza una transición suave:

```text
s <= 0,85 · blanco  -> confianza = 1
s >= blanco         -> confianza = 0
zona intermedia     -> smoothstep entre 1 y 0
```

Así se reducen discontinuidades cuando dos exposiciones se relevan en una alta
luz.

## 5. Alineamiento geométrico

### 5.1. Referencia común

Todas las exposiciones se alinean directamente contra la más oscura. No se
encadenan transformaciones entre capturas consecutivas, ya que los pequeños
errores se acumularían a lo largo del bracket.

### 5.2. Primera etapa: MTB entero

El alineamiento siempre comienza con Median Threshold Bitmaps. Para cada imagen
se construye una pirámide de seis niveles mediante promedios `2x2`. En cada nivel:

1. se calcula un umbral relacionado con la mediana luminosa;
2. se genera un bitmap por encima y por debajo del umbral;
3. se excluyen los valores próximos al umbral, más sensibles a ruido;
4. se prueban desplazamientos vecinos alrededor de la solución heredada del
   nivel anterior;
5. se elige la traslación con menos desacuerdos binarios.

MTB tolera diferencias grandes de exposición porque compara estructura, no
igualdad radiométrica exacta.

### 5.3. Modos disponibles

```text
integer   termina después de MTB; no interpola muestras
subpixel  refina únicamente la traslación mediante ECC
affine    refina traslación, rotación, escala y shear mediante ECC
```

`subpixel` es el modo predeterminado. `affine` es experimental y resulta útil
cuando existe una rotación o variación geométrica pequeña que una traslación no
puede explicar.

### 5.4. Imagen auxiliar para ECC

ECC no opera directamente sobre el mosaico a resolución completa. Se crea una
previsualización a un cuarto de tamaño, se normaliza por su media, se aplica
`log(1+x)` para comprimir las diferencias de EV y se suaviza con un gaussiano.

OpenCV optimiza entonces la correlación ECC partiendo de la traslación MTB. La
transformación sólo se acepta si:

- la correlación final es al menos `0,45`;
- la traslación no supera el 20 % de la imagen;
- en modo afín, el determinante es positivo;
- las escalas están entre `0,97` y `1,03`;
- la rotación no supera `5°`.

Si ECC falla o la solución no es razonable, se conserva el alineamiento entero.

### 5.5. Remuestreo CFA-safe

Para Bayer `2x2`, cada fase se trata como una imagen independiente:

```text
R  G1       plano R, plano G1,
G2 B   ->   plano G2 y plano B
```

Al producir una coordenada de salida, el remuestreador sólo interpola cuatro
vecinos de la misma fase mediante interpolación bilineal. Nunca promedia rojo y
verde, por ejemplo. La transformación se aplica como mapa inverso desde la
coordenada de destino hacia la fuente.

Una muestra cuyo soporte cae fuera de la captura se marca como inválida. El cero
utilizado para rellenar memoria no se confunde con negro fotográfico porque el
mapa de validez acompaña a la imagen.

La interpolación bilineal es conservadora, pero puede suavizar detalle y crear
correlación espacial entre muestras vecinas. Esta correlación no equivale a una
incertidumbre de registro y no se usa actualmente como varianza dentro de IRLS.

### 5.6. Recorte

Cuando el recorte está activado se busca el rectángulo de mayor área válido en
todas las exposiciones. Esto elimina tanto bordes por traslación como triángulos
vacíos creados por una transformación afín.

Con `--no-crop` se conserva el lienzo, pero algunas exposiciones pueden no ser
válidas cerca del borde; las etapas posteriores consultan siempre esa validez.

## 6. Normalización radiométrica

### 6.1. Por qué es necesaria

Después de alinear, `4000` unidades RAW en una captura corta no representan la
misma radiancia que `4000` unidades en una captura larga. Todas las muestras se
convierten mediante una función de respuesta `f_k` a un dominio radiométrico
común:

```text
E_k(x,y) = f_k(RAW_k(x,y))
```

La implementación actual usa una escala global por exposición. No estima una
escala independiente para cada canal CFA.

### 6.2. Relaciones por pares

Para cada par con solapamiento suficiente se estima el logaritmo de la razón de
exposición. El área se divide en una cuadrícula `16x16`. Dentro de cada tile se
usan muestras:

- de la misma fase CFA;
- por encima de un suelo de ruido;
- por debajo del 90 % de los blancos respectivos;
- válidas en ambas imágenes.

Cada tile produce la mediana de sus log-ratios. La mediana global y una pasada
Huber reducen la influencia de movimiento, reflejos y cambios locales. También
se calcula una dispersión robusta, que más adelante participa en la varianza de
fusión.

### 6.3. Grafo global

Las relaciones válidas forman un grafo cuyas imágenes son nodos y cuyas
comparaciones son aristas:

```text
imagen 0 ---- imagen 1
   |             |
   +---------- imagen 2 (ancla oscura)
```

Se resuelve conjuntamente una escala para toda la pila, fijando la imagen más
oscura como ancla. Cuatro iteraciones robustas reducen el peso de una relación
por pares incompatible con las demás. Esto evita acumular errores al multiplicar
razones consecutivas.

Si el grafo no contiene información suficiente, se vuelve a la estimación
robusta entre imágenes adyacentes.

## 7. Máscara de exposición

### 7.1. Significado

La máscara no es una opacidad ni una medida directa de cuánto aporta cada RAW.
En cada coordenada guarda el índice de la **primera exposición segura** y define
la fuente de fallback:

```text
máscara = 0  -> la exposición más larga todavía es segura
máscara = 1  -> la 0 está saturada o no es válida; empezar en la 1
máscara = 2  -> usar como mínimo la exposición más oscura
```

Por tanto, una región asignada a la capa 0 puede recibir información de las
capas 0, 1 y 2 durante la fusión. Una región asignada a la capa 1 puede usar 1 y
2, pero no debe recuperar automáticamente la capa 0 saturada.

### 7.2. Generación automática

Para cada coordenada se recorre la pila desde la más expuesta hasta encontrar la
primera imagen que:

- contiene una muestra geométricamente válida;
- no está saturada en su entorno `3x3`.

El resultado se conserva también como `origMask`. Esta copia permite distinguir
una decisión automática de una edición posterior del usuario.

### 7.3. Deghosting opcional

Con un umbral mayor que cero, la exposición segura se compara con la referencia
oscura en el dominio radiométrico común. La diferencia relativa debe superar
tanto el umbral solicitado como un suelo dependiente del ruido:

```text
diferencia_relativa = 100 · |E_segura - E_oscura| / señal
suelo_ruido          = 100 · 4 · sqrt(señal) / señal
```

Un solo píxel discrepante no basta para declarar movimiento. Se exige soporte en
un vecindario `3x3` recorrido con paso dos, por lo que todos los votos pertenecen
a la misma fase Bayer. Después se dilata el núcleo una muestra CFA para cubrir
el borde antialias del objeto.

Las zonas confirmadas como movimiento se asignan a la exposición más oscura y
se bloquean contra la fusión temporal. Esto prioriza una única geometría frente
a la reducción de ruido.

### 7.4. Edición manual

El pincel cambia índices de capa, no pinta valores RAW ni desenfoca la imagen.
Cada trazo sólo modifica posiciones donde la capa de destino es válida y queda
registrado para deshacer y rehacer.

Una selección manual se considera una decisión explícita: la fusión temporal se
desactiva en ese píxel y se permite anular la protección automática contra una
capa más luminosa. El feathering espacial todavía puede suavizar el borde del
trazo según el radio elegido al guardar.

## 8. Composición espacial

Antes de la fusión temporal se construye una composición base. Para cada capa se
crea un mapa binario independiente:

```text
M_k(x,y) = 1 si máscara(x,y) == k; 0 en otro caso
```

Cada `M_k` se desenfoca con el radio de feathering. Desenfocar mapas one-hot por
separado evita un error clásico: desenfocar directamente números de capa podría
inventar una capa intermedia que nunca fue seleccionada.

Los pesos se filtran además por:

- validez geométrica;
- saturación;
- restricción de no volver a una exposición anterior a `origMask`;
- bloqueo de movimiento automático;
- compatibilidad radiométrica con la capa elegida.

La guía radiométrica atenúa suavemente una capa incompatible:

```text
sigma = max(32, 0,03 · señal + 3 · sqrt(señal))
r     = |E_k - E_elegida| / sigma
peso  = peso_espacial / (1 + r^4)
```

La composición base es:

```text
fallback(x,y) = sum(peso_k · E_k) / sum(peso_k)
```

Si no queda ningún peso, se usa la capa elegida o, en último término, la primera
imagen válida encontrada.

## 9. Fusión temporal

### 9.1. Tres modos

```text
robust  IRLS sensible al ruido y a la coherencia; predeterminado
legacy  promedio robusto anterior, más permisivo y suavizante
off     conserva únicamente la composición espacial y la máscara
```

La fusión se omite siempre en movimiento automático y en píxeles editados
manualmente.

### 9.2. Conjunto de observaciones

En un píxel automático y estático se consideran todas las exposiciones desde:

```text
primera = max(máscara_actual, máscara_original)
```

hasta la más oscura. Una muestra entra si es válida y tiene confianza de
saturación positiva. De este modo ninguna exposición marcada como insegura se
reintroduce silenciosamente.

### 9.3. Modelo de incertidumbre actual

Para cada observación normalizada `E_k` se aproxima:

```text
escala_k             = E_k / RAW_k
varianza_sensor      = escala_k^2 · (RAW_k + 4^2)
varianza_respuesta   = E_k^2 · dispersión_log_k^2
varianza_total       = varianza_sensor + varianza_respuesta
```

El primer término es un modelo Poisson-Gaussiano genérico: ruido de disparo más
un suelo conservador de lectura de cuatro unidades RAW. El segundo expresa la
incertidumbre observada al estimar la relación radiométrica de esa exposición.

No es todavía un perfil físico específico de cámara e ISO. Tampoco incluye una
covarianza geométrica ECC. Añadir una varianza de registro correctamente
formulada requeriría propagar la covarianza de la transformación a cada píxel:

```text
Sigma_xy = J_warp · Cov(parametros_ECC) · J_warp^T
var_reg  = gradiente^T · Sigma_xy · gradiente
```

Hasta disponer de una covarianza validada, es preferible omitir ese término que
aproximarlo mediante la fase de interpolación. Esa aproximación anterior hacía
variar la confianza siguiendo la rejilla CFA y podía revelar una textura
periódica después del demosaicing.

### 9.4. IRLS robusto

El centro inicial es la mediana ponderada por saturación e inversa de varianza.
Después se ejecutan hasta cuatro iteraciones IRLS con la función biweight de
Tukey:

```text
corte_k = 4,685 · sqrt(varianza_k)
u_k     = |E_k - centro| / corte_k

Tukey(u) = (1 - u^2)^2   si u < 1
           0             si u >= 1

peso_k = confianza_saturación_k · Tukey(u_k) / varianza_k
centro = sum(peso_k · E_k) / sum(peso_k)
```

Una observación muy discordante termina con peso cero. Una exposición larga y
limpia suele tener más peso que una corta y ruidosa, siempre que no esté próxima
a saturación y sea coherente con el resto.

### 9.5. Consenso y fallback

No basta con obtener una media: se mide cuántas observaciones efectivas la
sostienen:

```text
N_efectivo = (sum(peso_k))^2 / sum(peso_k^2)
confianza  = limitar(N_efectivo - 1, 0, 1)
resultado  = fallback + confianza · (centro_IRLS - fallback)
```

- con al menos dos candidatas, una única observación efectiva produce confianza
  cero y recupera el fallback;
- dos observaciones con pesos parecidos se acercan a confianza uno;
- los casos intermedios vuelven gradualmente al fallback.

Si sólo existe una muestra candidata válida desde el principio, se utiliza esa
muestra directamente. Si no existe ninguna, se conserva el fallback espacial.

Este mecanismo es especialmente importante con sólo dos exposiciones: si se
contradicen, no existe mayoría que permita decidir cuál representa la escena.

### 9.6. Modo Legacy

`legacy` calcula una mediana simple, elige como ancla la observación más próxima
y acepta valores dentro de un corte que incluye:

```text
5 · sqrt(var_muestra + var_ancla) + 1 % de la señal
```

Suele producir más suavizado y puede reducir algo más el ruido, pero esa
tolerancia proporcional a la señal admite discrepancias mayores en altas luces.
Se conserva para comparación y compatibilidad experimental.

## 10. Qué aporta cada exposición

Considérese un bracket de tres capturas `+2 EV`, `0 EV` y `-2 EV`:

```text
Sombra estática:
  +2 EV válida  -> máscara +2 EV; las tres pueden contribuir

Tono medio:
  +2 EV cerca del blanco -> su confianza cae; 0 EV y -2 EV ganan peso

Alta luz:
  +2 EV saturada -> máscara 0 EV; sólo 0 EV y -2 EV pueden contribuir

Reflejo extremo:
  +2 EV y 0 EV saturadas -> máscara -2 EV; sólo queda -2 EV

Objeto en movimiento:
  deghosting -> una sola fuente geométrica, sin promedio temporal
```

El resultado puede contener un rango dinámico mayor que cualquiera de las
entradas porque conserva simultáneamente señal limpia de la exposición larga y
altas luces no recortadas de la corta. Cuando las exposiciones están muy juntas
y ninguna aporta altas luces adicionales, no aumenta apenas el rango capturado,
pero las observaciones coherentes todavía pueden reducir ruido.

## 11. Escalado y rango de salida

La composición se mantiene en `float` hasta la escritura. Al final se aplica uno
de dos escalados:

```text
normal:
    multiplicador = blanco_salida / máximo_compuesto

--preserve-exposure:
    multiplicador = blanco_salida / 65535
```

El modo normal aprovecha el rango numérico disponible para cada resultado. El
modo `preserve-exposure` evita normalizar cada bracket de forma independiente y
resulta útil para series destinadas a panoramas.

El rango físico procede de las observaciones no saturadas, no de crear valores
arbitrarios por encima de los RAW. La escala final sólo elige cómo codificar esa
radiancia común dentro del DNG.

## 12. Preview y DNG

HDRMerge genera una previsualización RGB para la GUI y para las imágenes
embebidas, pero ésta no reemplaza al mosaico principal. El DNG guarda:

- una muestra CFA por coordenada;
- valores flotantes de 16, 24 o 32 bits;
- compresión Deflate y predictor flotante;
- patrón y dimensiones CFA;
- blanco de salida y negro cero;
- área activa, recorte y orientación;
- matriz de color, balance de blancos y metadatos transferidos;
- miniatura y, opcionalmente, preview JPEG.

La escritura es transaccional. Primero se genera un archivo temporal, se
comprueba que no esté vacío y sólo entonces sustituye al destino. Si ya existía
una salida, se conserva como backup hasta completar el reemplazo. Nunca se
permite sobrescribir uno de los RAW de entrada.

## 13. Determinismo

Con los mismos archivos, opciones, versión y plataforma, el proceso está
diseñado para producir el mismo resultado. El ruido no se genera ni se utiliza
aleatoriedad durante la fusión.

Puede haber diferencias numéricas mínimas entre compiladores, arquitecturas o
versiones de OpenCV por el orden de operaciones en coma flotante y la
convergencia ECC. No deberían convertirse en diferencias fotográficas grandes;
si lo hacen, indican una inestabilidad que debe investigarse.

## 14. Artefactos: dónde pueden originarse

### 14.1. Arco o cambio cromático en una costura

Posibles causas:

- escala radiométrica imprecisa entre exposiciones;
- transición de saturación demasiado abrupta;
- feathering entre muestras incompatibles;
- edición de máscara sobre una fuente saturada.

Las escalas globales robustas, la confianza suave de saturación y la guía
radiométrica del feathering atacan específicamente estas causas.

### 14.2. Retícula, laberinto o textura plástica

Posibles causas:

- correlación introducida por el remuestreo CFA bilineal;
- pesos temporales que cambian siguiendo la fase de interpolación;
- reducción de ruido y enfoque agresivos durante el revelado;
- demosaicing que amplifica una correlación ya presente en el mosaico.

La revisión actual eliminó el uso de la fase fraccionaria como falsa varianza de
registro. En el ensayo sintético, la variación media de confianza entre vecinos
de la misma fase bajó de `0,026` a `0,0054`, manteniendo la reducción de ruido.
La correlación inherente al remuestreo no desaparece por completo.

### 14.3. Doble borde o pérdida de detalle

Posibles causas:

- registro residual;
- paralaje o movimiento no modelable mediante una transformación global;
- deghosting desactivado o demasiado permisivo;
- promedio de una muestra remuestreada con otra de geometría diferente.

IRLS rechaza discrepancias radiométricas claras, pero no sustituye a un buen
alineamiento. Una diferencia pequeña puede quedar dentro del corte estadístico
y producir una suavidad leve.

## 15. Controles principales

| Objetivo | GUI/CLI | Efecto |
|---|---|---|
| Evitar toda interpolación | `--alignment integer` | Sólo traslación entera MTB |
| Alineamiento recomendado | `--alignment subpixel` | Traslación ECC y remuestreo CFA |
| Corregir rotación/escala pequeñas | `--alignment affine` | Transformación afín experimental |
| Desactivar alineamiento | `--no-align` | Conserva coordenadas originales |
| Detectar movimiento | `--deghost N` | Fuerza una fuente en regiones coherentemente discrepantes |
| Fusión actual | `--fusion robust` | IRLS, predeterminado |
| Fusión anterior | `--fusion legacy` | Más permisiva y suavizante |
| Sólo máscara | `--fusion off` | Sin reducción temporal de ruido |
| Ajustar la costura | radio de feathering | Suaviza mapas espaciales de capa |
| Conservar escala entre series | `--preserve-exposure` | Evita normalización independiente |
| Conservar bordes completos | `--no-crop` | No recorta al rectángulo común |

## 16. Fallos y fallback

El pipeline intenta degradarse de forma explícita:

```text
ECC inválido            -> alineamiento MTB entero
grafo radiométrico débil -> relaciones robustas adyacentes
muestra saturada         -> exposición más oscura
consenso IRLS insuficiente -> composición de máscara
movimiento confirmado    -> una sola fuente
escritura fallida         -> destino anterior intacto
```

Esto no garantiza una imagen perfecta, pero evita que una etapa sofisticada
destruya silenciosamente una solución más sencilla y válida.

## 17. Limitaciones actuales

- El remuestreo subpíxel sólo es CFA-safe para Bayer repetitivo `2x2`.
- La geometría es global; no modela paralaje, rolling shutter o deformaciones
  locales.
- La interpolación bilineal puede suavizar y correlacionar ruido.
- El modelo de ruido no usa todavía perfiles medidos por cámara e ISO.
- ECC entrega una puntuación de correlación, no una covarianza de parámetros;
  por eso no se propaga todavía una varianza geométrica rigurosa.
- El estimador radiométrico usa una escala por exposición, no por canal CFA.
- Linear DNG no dispone de una ruta RGB específica.
- El escritor mantiene la salida completa en memoria.

## 18. Mapa del código

| Etapa | Implementación principal |
|---|---|
| Orquestación de carga y guardado | `src/ImageIO.cpp` |
| Metadatos y geometría RAW | `src/RawParameters.cpp` |
| Representación de una exposición | `src/Image.cpp` |
| Orden, respuesta, máscara y fusión | `src/ImageStack.cpp` |
| MTB | `src/Image.cpp`, `src/Bitmap.cpp` |
| ECC y remuestreo CFA | `src/CfaAlignment.cpp` |
| Máscara editable | `src/EditableMask.cpp` |
| Preview | `src/ImageIO.cpp` |
| Escritura DNG | `src/DngFloatWriter.cpp` |
| Transferencia EXIF | `src/ExifTransfer.cpp` |
| Opciones GUI/CLI | `src/LoadSaveOptions.hpp`, `src/Launcher.cpp` |
| Regresiones CFA | `test/testCfaAlignment.cpp` |
| Regresiones de composición | `test/testMergeQuality.cpp` |

## 19. Resumen mental

La forma más útil de recordar el pipeline es:

```text
alinear     -> poner la misma escena en la misma coordenada
normalizar  -> expresar todas las capturas en la misma escala radiométrica
seleccionar -> encontrar la primera exposición válida y no saturada
proteger    -> separar movimiento y decisiones manuales
feathering  -> suavizar espacialmente los cambios de fuente
fusionar    -> combinar sólo observaciones coherentes y cuantificar su consenso
codificar   -> conservar el resultado como mosaico CFA dentro de un DNG
```

La máscara establece seguridad y fallback; no determina por sí sola toda la
contribución. IRLS aporta reducción de ruido; no inventa rango dinámico donde
ninguna entrada contiene señal. El rango adicional aparece porque exposiciones
distintas aportan observaciones válidas en partes distintas de la escala tonal.
