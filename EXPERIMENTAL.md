# HDRMerge 0.6.0 experimental

## 1. Propósito y alcance

Esta rama es una evolución experimental de `release-v0.6`. Su objetivo es
modernizar el comportamiento funcional de HDRMerge sin abandonar su principio
fundamental: combinar exposiciones directamente en el dominio RAW y producir un
DNG que conserve un mosaico CFA interpretable por un revelador externo.

La comparación documentada aquí toma como base el commit:

```text
4aac9f901b51dc2eb706d33d009f38318436d3e4  release-v0.6
```

La rama experimental es:

```text
codex/experimental-functional-overhaul
```

Los cambios abarcan estabilidad, alineamiento, composición HDR, máscaras,
procesamiento por lotes, interfaz, automatización CLI y distribución para
Windows. No debe considerarse todavía una versión estable: conviene conservar
siempre los RAW originales y comprobar visualmente los resultados importantes.

## 2. Principios técnicos

Las decisiones de diseño siguen estos principios:

### 2.1. Preservar la semántica RAW

Una muestra de un sensor con CFA no es un píxel RGB completo. Cada posición
representa únicamente una fase del patrón —por ejemplo `R`, `G1`, `G2` o `B`—.
Las transformaciones geométricas refinadas nunca interpolan directamente el
mosaico como si fuera una imagen monocroma convencional.

### 2.2. Refinamiento progresivo

El alineamiento más complejo parte de una solución sencilla y robusta:

```text
MTB entero → refinamiento ECC → validación geométrica → remuestreo CFA
```

Si una etapa refinada no es fiable, se conserva el resultado entero. Una mejora
experimental no debe destruir una solución válida.

### 2.3. Referencia común

Todas las exposiciones se alinean directamente contra una misma referencia. No
se encadenan transformaciones entre imágenes consecutivas, porque esa estrategia
acumula error y puede terminar desplazando la última imagen varios píxeles.

### 2.4. Validez explícita

Un píxel creado fuera de los límites de una imagen transformada no se representa
simplemente como negro: se marca como inválido. La máscara de validez participa
en la estimación radiométrica, la selección de exposición, la composición y el
recorte.

### 2.5. Separar geometría y radiometría

El cálculo de movimiento utiliza una representación normalizada para tolerar
diferencias de EV. La composición continúa trabajando con muestras RAW lineales
y sus funciones de respuesta.

### 2.6. Fallar de forma segura y observable

Los errores se propagan hasta GUI y CLI, las operaciones cancelables liberan el
estado parcial y la escritura usa archivos temporales. Los modos detallados
informan de desplazamiento, rotación, confianza y fallback.

### 2.7. Valores predeterminados conservadores

El deghosting automático y el modo afín dependen mucho de la escena y se
mantienen como opciones explícitas. La fusión de exposiciones coherentes para
reducir ruido sí está activada por defecto: exige consenso radiométrico y vuelve
a la exposición seleccionada por la máscara cuando no puede establecerlo.

## 3. Versión e identificación

El proyecto original de la rama `release-v0.6` todavía declaraba internamente
la versión `0.5.0`. Se corrigió la configuración de CMake para mostrar:

```text
0.6.0-experimental
```

El paquete Windows incorpora además `BUILD-INFO.txt`, con rama, revisión,
toolchain y dependencias relevantes. Esto permite relacionar un ejecutable con
el código exacto que lo produjo.

## 4. Carga RAW y tratamiento de errores

### 4.1. Corrección de precedencia en las banderas de LibRaw

Se corrigió esta comprobación conceptual:

```cpp
!flags & LIBRAW_DECODER_FLATFIELD
```

La negación tenía precedencia sobre el AND binario. La forma correcta es:

```cpp
!(flags & LIBRAW_DECODER_FLATFIELD)
```

Sin los paréntesis, el programa podía clasificar incorrectamente la capacidad
del decodificador.

### 4.2. Entradas vacías, corruptas o incompatibles

La carga ahora detecta explícitamente:

- ausencia de archivos de entrada;
- RAW que no pueden abrirse o decodificarse;
- archivos sin fotogramas utilizables;
- geometría RAW diferente entre exposiciones;
- patrón CFA incompatible;
- cancelación solicitada por el usuario;
- contenedores con un número excesivo de fotogramas.

El estado de imágenes y metadatos se limpia cuando la carga no puede
completarse. Esto evita desreferenciar `rawParameters` vacío o continuar con una
pila parcialmente válida.

### 4.3. RAW multifotograma

El límite anterior de cuatro fotogramas se amplió a 32. Se mantienen casos como
RAW HDR de varias tomas o contenedores que LibRaw expone como imágenes internas,
pero se conserva un límite para evitar consumos de memoria incontrolados.

### 4.4. Cancelación real

`ProgressIndicator` dispone ahora de destructor virtual y de `isCanceled()`.
La GUI presenta un progreso modal con botón **Cancel** y procesa eventos de
usuario mientras el trabajo está en segundo plano.

La cancelación se comprueba durante la carga y antes de las fases costosas de
guardado. Si se cancela, no se utiliza el estado incompleto.

### 4.5. Extensiones de archivo

El selector RAW incorpora explícitamente:

- Canon CR3 (`.cr3`);
- GoPro GPR (`.gpr`);
- QuickTake (`.qtk`);
- un filtro **All files** para formatos reconocidos por LibRaw que no estén en
  la lista histórica.

La compatibilidad efectiva continúa dependiendo de la versión y opciones de
compilación de LibRaw.

## 5. Alineamiento automático

El alineamiento dispone de tres modos:

```text
integer   → traslación de píxeles enteros compatible con el algoritmo histórico
subpixel  → traslación con precisión fraccionaria; modo predeterminado
affine    → traslación, rotación, escala y shear limitado; experimental
```

### 5.1. Orden y referencia

La pila se ordena desde la exposición más luminosa hasta la menos luminosa. La
imagen menos expuesta actúa actualmente como referencia común. Todas las demás
se registran contra ella de manera independiente.

La referencia menos expuesta reduce el problema de las altas luces saturadas,
aunque puede contener más ruido. La selección automática o manual de otra
referencia es una mejora futura posible.

### 5.2. Primera etapa: Median Threshold Bitmap

El alineamiento entero construye una pirámide de seis niveles. Cada nivel reduce
la imagen combinando bloques `2×2`:

```text
resolución completa → 1/2 → 1/4 → 1/8 → …
```

En cada escala:

1. se obtiene un umbral mediante el histograma;
2. se crea un bitmap de zonas claras y oscuras;
3. se excluyen valores próximos al umbral, menos estables ante ruido y EV;
4. se evalúan las nueve posiciones de un vecindario `3×3`;
5. se escoge la posición con menor XOR en la zona válida;
6. el desplazamiento se propaga a la escala siguiente.

El algoritmo mantiene desplazamientos compatibles con la periodicidad Bayer,
por lo que no cambia accidentalmente una posición roja por una verde o azul.

### 5.3. Imagen auxiliar para el refinamiento

Los modos `subpixel` y `affine` construyen una vista a un cuarto de resolución.
Para cada bloque `4×4` se calcula un promedio y después se aplican:

1. normalización por la media de la imagen;
2. transformación `log(1 + x)`;
3. desenfoque gaussiano `5×5`.

La normalización reduce diferencias multiplicativas entre exposiciones. El
logaritmo comprime el rango dinámico y el filtro gaussiano atenúa ruido y la
estructura de alta frecuencia del CFA. Esta vista solo sirve para calcular la
geometría; no sustituye las muestras RAW de salida.

### 5.4. Estimación ECC con OpenCV

La traslación MTB se convierte en la estimación inicial para
`cv::findTransformECC()`:

- `subpixel` utiliza `cv::MOTION_TRANSLATION`;
- `affine` utiliza `cv::MOTION_AFFINE`;
- máximo de 80 iteraciones;
- terminación por error `1e-6`;
- filtrado gaussiano interno de tamaño 5.

ECC maximiza la correlación entre la imagen de referencia y la imagen de
entrada. Al iniciarse cerca de la solución entera, no necesita buscar una
transformación arbitraria desde cero.

La matriz calculada a un cuarto de resolución se convierte a coordenadas RAW
completas teniendo en cuenta el centro de los bloques. La transformación
almacenada es un mapa inverso:

```text
coordenada de salida/referencia → coordenada que debe muestrearse en el RAW origen
```

### 5.5. Validación y fallback

Una transformación refinada solo se acepta si cumple todas las comprobaciones:

- valores finitos;
- confianza ECC igual o superior a `0,45`;
- desplazamiento inferior al 20 % de cada dimensión;
- determinante positivo;
- en modo afín, escala por eje dentro de `0,97–1,03`;
- en modo afín, rotación absoluta inferior a `5°`.

Si OpenCV no está disponible, ECC no converge o la matriz excede esos límites,
se conserva sin modificar el alineamiento entero anterior.

### 5.6. Remuestreo CFA-safe

Un RAW Bayer `2×2` puede representarse como cuatro retículas entrelazadas:

```text
R   G1
G2  B
```

Aplicar interpolación bilineal sobre el mosaico completo mezclaría colores. El
nuevo remuestreador identifica la fase de cada píxel de salida y transforma sus
coordenadas a la retícula correspondiente:

```text
R  ← exclusivamente muestras R
G1 ← exclusivamente muestras G1
G2 ← exclusivamente muestras G2
B  ← exclusivamente muestras B
```

Dentro de cada retícula se realiza interpolación bilineal con las cuatro
muestras vecinas. El valor se redondea y limita al rango de `uint16_t`.

La distinción entre `G1` y `G2` es deliberada: aunque ambas posiciones midan
verde, ocupan fases espaciales diferentes y no deben intercambiarse durante el
remuestreo RAW.

### 5.7. Máscara de validez geométrica

Cada píxel remuestreado lleva una marca de validez. Si alguna muestra necesaria
queda fuera del RAW de origen, el valor se inicializa a cero pero se marca como
inválido. La marca, no el cero, determina si puede participar en el HDR.

Las funciones de respuesta, detección de saturación, máscara HDR y promedio de
exposiciones consultan esa validez antes de leer una muestra.

### 5.8. Recorte después de una transformación afín

Una rotación produce esquinas triangulares sin datos. El recorte anterior solo
calculaba la intersección de rectángulos desplazados y no podía representar
estas zonas.

Ahora se forma la intersección de las máscaras válidas y se busca, en tiempo
lineal respecto al número de píxeles, el rectángulo de mayor área compuesto
enteramente por muestras válidas. Se utiliza el algoritmo de rectángulo máximo
sobre histogramas acumulados por fila.

### 5.9. Información de diagnóstico

Con `-vv` se muestran, para cada exposición:

- desplazamiento inicial entero;
- desplazamiento refinado con decimales;
- rotación estimada;
- confianza ECC;
- razón de rechazo cuando se utiliza el fallback.

La barra de estado de la GUI también muestra desplazamiento, rotación y
confianza de la capa situada bajo el cursor.

## 6. Selección de saturación y precisión radiométrica

### 6.1. Umbral blanco en el dominio correcto

La detección se realiza después de sustraer el nivel negro. Por tanto, el nivel
blanco declarado por LibRaw se convierte al mismo dominio:

```text
blanco_útil = blanco_cámara - negro_máximo
```

Ese valor es la referencia principal. Un máximo observado solo puede refinarlo
si alcanza al menos el 90 % del blanco útil y es, por tanto, compatible con una
meseta de recorte real. Una zona plana o un canal CFA que no llegue a saturarse
ya no puede hundir el umbral común hasta un valor ordinario de la escena.

Salvo que el usuario proporcione un nivel blanco personalizado, se conserva un
margen de seguridad del 1 %.

### 6.2. Nivel negro del DNG flotante

Las muestras de entrada tienen su nivel negro sustraído antes de calcular la
respuesta. La salida compuesta representa por tanto señal útil con cero físico
en cero.

El DNG resultante declara niveles negros iguales a cero. No se vuelve a sumar el
nivel negro del RAW original, lo que evita desplazar las sombras y conserva
mejor la precisión del DNG flotante.

### 6.3. Escala radiométrica global robusta

Para cada pareja de exposiciones se estiman razones sobre su solapamiento
válido. Solo se comparan muestras de la misma fase CFA, por encima del suelo de
ruido y por debajo del 90 % de sus umbrales de saturación. El área se divide en
una cuadrícula `16 x 16`; cada celda aporta la mediana de las razones en dominio
logarítmico, evitando que una región extensa, texturada o en movimiento domine
la estimación.

Las medianas se consolidan mediante MAD y un refinamiento Huber. Después, todas
las relaciones válidas entre parejas se resuelven conjuntamente para obtener
una única escala por imagen. Cuatro iteraciones IRLS reducen el peso de una
pareja incompatible con el resto de la pila. Así no se acumula error al encadenar
exposiciones consecutivas y no se introducen ganancias independientes por canal
CFA.

La dispersión robusta de las relaciones por tiles y el residuo de cada arista
respecto a la solución global se conservan como incertidumbre radiométrica de
cada imagen. No solo se estima cuánto hay que escalar una exposición: también
se mide hasta qué punto esa escala describe uniformemente el solapamiento.

Si el grafo de relaciones no conecta toda la pila, se emplean como fallback las
estimaciones robustas entre imágenes adyacentes.

### 6.4. Transición antes de saturación

Una muestra conserva peso completo hasta el 85 % del umbral. Entre el 85 % y el
100 % su confianza decrece suavemente mediante una curva *smoothstep* y, al
alcanzar el umbral, queda excluida de la fusión adaptativa. Esto evita que sus
pesos cambien bruscamente en el borde de una alta luz.

## 7. Deghosting automático

El deghosting es opcional y se ejecuta al construir la máscara inicial.

Para el píxel candidato de una exposición se compara su valor normalizado con
la exposición común de referencia:

```text
diferencia (%) = 100 · |a - b| / max(1, |a|, |b|)
```

La decisión ya no depende únicamente del umbral indicado. En sombras se añade
un suelo adaptativo proporcional a la raíz cuadrada de la señal, aproximación
del ruido de disparo. Así, una fluctuación pequeña sobre señal débil no se
convierte automáticamente en movimiento.

La detección inicial se consolida espacialmente por separado en los cuatro
planos Bayer. Se consulta un vecindario `3 × 3` con paso de dos píxeles, de modo
que todos los votos corresponden al mismo color CFA. Se exigen al menos tres
votos y el núcleo aceptado se expande una muestra CFA para cubrir los bordes del
objeto. Los puntos aislados —ruido, hot pixels o pequeñas imprecisiones— quedan
descartados. En el núcleo coherente se utiliza exclusivamente la exposición de
referencia; el feathering no vuelve a introducir allí otras exposiciones.

Una edición manual posterior tiene prioridad: si el usuario modifica un píxel
de la máscara automática, la composición respeta esa corrección.

Se controla mediante:

```bash
--deghost N       # N entre 1 y 100
```

En la GUI existe una casilla de activación y un selector porcentual. Permanece
desactivado por defecto porque una máscara global no puede resolver correctamente
todos los casos de movimiento, oclusión, transparencia o reflejos.

### 7.1. Fusión mediante pesos por exposición

La versión original suavizaba directamente el índice entero de la máscara. Un
valor desenfocado de `1.4`, por ejemplo, significaba mezclar las capas 1 y 2.
Esto solo es matemáticamente seguro cuando todas las fronteras conectan capas
consecutivas. Una frontera directa entre las capas 0 y 3 podía atravesar las
capas 1 y 2 aunque nunca hubieran sido seleccionadas.

La rama experimental construye ahora una máscara *one-hot* independiente para
cada exposición:

```text
M_k(x,y) = 1 si la máscara selecciona k; 0 en otro caso
```

Cada `M_k` se suaviza por separado y solo después se combinan y normalizan los
pesos válidos:

```text
resultado(x,y) = Σ w_k(x,y) · E_k(x,y) / Σ w_k(x,y)
```

Una exposición queda excluida si no contiene el píxel, está saturada en su
entorno o es anterior a la primera exposición segura. La selección manual puede
anular esta última protección de forma explícita.

El peso espacial se modula además por la semejanza radiométrica con la capa
seleccionada. La penalización tiene forma suave de cuarto orden:

```text
w'_k = w_k / (1 + (|E_k - E_ref| / σ)^4)
σ    = max(32, 0.03 · señal + 3 · √señal)
```

En una región estática, las exposiciones normalizadas son semejantes y el
feathering funciona normalmente. En el contorno de un objeto desplazado, una
muestra radiométricamente incompatible pierde peso en vez de producir una
doble silueta. Todo ello opera sobre la muestra CFA de la misma coordenada, sin
mezclar colores del mosaico.

## 8. Fusión adaptativa para reducción de ruido

La fusión dispone de tres modos seleccionables con `--fusion MODE`:

```text
robust   nueva IRLS sensible a incertidumbre; predeterminado
legacy   estimador anterior, más permisivo y con mayor suavizado
off      composición basada únicamente en selección y feathering
```

`--average` se conserva como alias de `--fusion robust` y `--no-average` como
alias de `--fusion off`, de modo que los scripts existentes mantienen su
comportamiento previsto.

En regiones estáticas se consideran, desde la primera exposición segura
seleccionada por la máscara, todas las imágenes que contienen el píxel. Cada
muestra se transforma al dominio radiométrico común y recibe una confianza de
saturación según la transición descrita en 6.4. La incertidumbre total combina
dos términos medibles:

```text
varianza_sensor    ≈ escala_respuesta² · (muestra_RAW + 4²)
varianza_respuesta ≈ radiancia² · dispersión_log_respuesta²
varianza_total     = varianza_sensor + varianza_respuesta
```

El término `4²` es un suelo conservador de ruido de lectura de cuatro unidades
RAW. No sustituye un perfil medido de la cámara, pero permite ponderar más las
observaciones con menor ruido estimado.

Una revisión posterior retiró un tercer término que multiplicaba la fase
fraccionaria del remuestreo bilineal por el gradiente CFA. La fase de
interpolación es determinista y no constituye por sí misma una varianza de
registro. Tratarla como incertidumbre espacial hacía variar los pesos IRLS con
la rejilla Bayer, especialmente en gradientes suaves y transformaciones afines,
y podía convertir ruido ordinario en una textura periódica después del
demosaicing. Las discrepancias causadas por un registro realmente incorrecto
continúan limitadas por el residuo radiométrico y los pesos Tukey.

El centro inicial es la mediana ponderada por saturación y varianza. Cuatro
iteraciones IRLS vuelven a estimarlo mediante pesos Tukey:

```text
corte_k = 4.685 · √varianza_total_k
peso_k  = confianza_saturación · Tukey(residuo_k / corte_k) / varianza_total_k
```

En modo `robust` se eliminó la tolerancia anterior proporcional al 1 % de la
señal, que podía aceptar discrepancias grandes precisamente en altas luces y
detalle brillante.
El consenso final se cuantifica mediante el tamaño efectivo de muestra:

```text
N_efectivo = (Σ peso_k)² / Σ peso_k²
confianza  = limitar(N_efectivo - 1, 0, 1)
resultado  = fallback_máscara + confianza · (centro_robusto - fallback_máscara)
```

Una sola observación efectiva produce confianza cero; dos observaciones con
pesos semejantes se aproximan a confianza uno. Los casos intermedios retornan
gradualmente a la exposición seleccionada por la máscara, en vez de tomar una
decisión binaria. Con dos observaciones discrepantes no hay información
suficiente para decidir cuál representa la escena y se conserva el fallback.

Las zonas detectadas como movimiento y los píxeles editados manualmente siempre
mantienen una sola fuente. Por tanto, la máscara no implica que las demás
exposiciones dejen de aportar información: fija la exposición segura de partida
y el fallback, mientras las exposiciones coherentes adicionales reducen ruido.

En la GUI aparece en las propiedades del DNG como **Fusion mode**, con las
opciones **Robust (detail preserving)**, **Legacy (stronger smoothing)** y
**Off (mask only)**. Puede guardarse como valor predeterminado. Una preferencia
antigua `averageSamples=true` migra a `robust`, y `false` migra a `off`.

## 9. Preservación de exposición entre series

Por defecto HDRMerge escala cada resultado para aprovechar el rango disponible
en función del máximo observado en su propia pila. Esto funciona bien para una
imagen aislada, pero varias series destinadas a un panorama pueden terminar con
brillos incompatibles.

`--preserve-exposure` utiliza una escala absoluta basada en el máximo declarado
por el RAW y `65535`, en lugar de normalizar independientemente por el máximo de
cada composición. Así se conserva mejor la relación luminosa entre distintos
brackets.

La GUI expone la misma opción como consistencia entre lotes. No sustituye una
calibración fotométrica completa, pero evita una normalización independiente
obvia.

## 10. Escritura segura de DNG

### 10.1. Propagación de errores

`DngFloatWriter::write()`, `Exif::transfer()` e `ImageIO::save()` devuelven ahora
un valor booleano. Un fallo de Exiv2, del escritor o del sistema de archivos ya
no se interpreta silenciosamente como éxito.

La CLI devuelve código distinto de cero y la GUI muestra un diálogo crítico con
el mensaje concreto.

### 10.2. Protección contra sobrescritura de entradas

Antes de guardar se comparan rutas canónicas y absolutas. El archivo de salida
no puede ser uno de los RAW utilizados como entrada, aunque se haya escrito con
otra forma relativa de la misma ruta.

### 10.3. Reemplazo transaccional

El nuevo flujo de escritura es:

```text
renderizar → escribir temporal → comprobar existencia/tamaño
           → renombrar salida anterior a backup
           → instalar temporal como salida
           → eliminar backup
```

Si no puede instalarse el temporal, se intenta restaurar el archivo anterior.
El temporal se elimina en los errores conocidos. De esta manera, un fallo a
mitad de escritura no sustituye inmediatamente un DNG válido por uno parcial.

La máscara PNG opcional se guarda después del DNG y comunica sus propios
errores. La transacción protege el DNG, pero no constituye una transacción única
entre DNG y archivo de máscara.

### 10.4. Validación previa

Se rechazan explícitamente:

- una pila vacía;
- un nombre de salida vacío;
- un directorio inexistente;
- una salida que coincide con una entrada;
- un temporal inexistente o de tamaño cero.

## 11. Máscaras editables

### 11.1. Exportación

La máscara de selección de capas puede guardarse como PNG desde la GUI o con
`-m`. Cada valor codifica qué exposición aporta el píxel correspondiente.

### 11.2. Importación

La GUI puede importar una máscara PNG siempre que sus dimensiones coincidan
exactamente con la composición actual.

- En PNG indexado se utiliza directamente el índice de paleta.
- En otros formatos se convierte la luminancia al intervalo de capas.
- Los valores se limitan entre cero y la última exposición disponible.

Después de importar se reinicia el historial editable para que la máscara
cargada actúe como nuevo punto de partida.

### 11.3. Avisos visuales

La previsualización puede mostrar:

- colores diferentes para cada capa;
- una vista neutra en escala de grises;
- en rojo, selecciones que utilizan una muestra saturada.

El aviso rojo ayuda a detectar manualmente zonas en las que la máscara debería
elegir una exposición más oscura.

## 12. Proyectos ligeros

La GUI puede guardar un proyecto `*.hdrmerge.json`. El JSON contiene:

- identificador y versión del formato;
- rutas absolutas de los RAW de origen;
- nombre del PNG lateral que contiene la máscara;
- porcentaje de zoom.

La máscara se guarda junto al proyecto con sufijo `.mask.png`. El JSON utiliza
`QSaveFile`, que escribe temporalmente y confirma el cambio al finalizar.

Al abrir un proyecto se recargan primero los RAW y después se restaura la
máscara. Se validan documento JSON, lista de archivos y dimensiones de la
máscara.

Es un formato deliberadamente pequeño, no un contenedor autocontenido: mover
los RAW rompe las rutas absolutas y obliga a corregirlas o recrear el proyecto.

## 13. Mejoras de previsualización y GUI

Sin constituir todavía el rediseño visual completo, se incorporaron mejoras de
uso inmediato:

- zoom de 10 % a 400 %;
- `Ctrl` + rueda del ratón para cambiar el zoom;
- **Fit preview** y atajo `Ctrl+0`;
- coordenadas de pincel corregidas según el factor de zoom;
- activación independiente de colores de capa y avisos de saturación;
- importación y exportación de máscara;
- apertura y guardado de proyectos;
- mensajes temporales de éxito y estado;
- errores visibles de carga y guardado;
- progreso cancelable;
- ajuste automático de la vista después de cargar;
- métricas de alineamiento en la barra de estado;
- posibilidad de usar `--gui -o patrón` para abrir la interfaz con una salida
  sugerida.

Las preferencias de alineamiento, deghosting, bits por muestra, preview,
suavizado, modo de fusión y preservación de exposición pueden persistirse mediante
`QSettings` cuando el diálogo correspondiente lo permite.

## 14. CLI y automatización

### 14.1. Nuevas opciones

```text
--gui                    fuerza la interfaz gráfica
--nogui                  fuerza el procesamiento por consola
--alignment MODE         integer, subpixel o affine
--deghost N              umbral porcentual de movimiento
--fusion MODE            robust, legacy u off
--average                alias de --fusion robust
--no-average             alias de --fusion off
--preserve-exposure      conserva escala entre brackets
--bracket-size N         limita cada grupo a N imágenes
```

Se conservan las opciones anteriores, entre ellas:

```text
-o ARCHIVO   -a              -B/--batch    -g SEGUNDOS
--single     -b 16|24|32     --no-align    --no-crop
-m MÁSCARA   -r RADIO        -p full|half|none
-v           -vv             -w NIVEL
```

### 14.2. Códigos de salida

Se corrigió una variable local que ocultaba el resultado global del proceso.
Ahora un error de carga o guardado se propaga hasta el código de salida del
ejecutable, lo que permite detectar fallos desde scripts y CI.

Una entrada inválida devuelve `1` y no deja un DNG parcial. Una ejecución
correcta devuelve `0`.

### 14.3. Entrada individual

Las imágenes individuales solo se omiten cuando está activo el modo batch y no
se ha pedido `--single`. Antes podían saltarse también fuera del procesamiento
por lotes, dejando sin procesar una invocación perfectamente válida.

### 14.4. Agrupación fija de brackets

`--bracket-size N` inicia un grupo nuevo al alcanzar `N` imágenes, además del
criterio temporal de `-g`. Es útil cuando los metadatos horarios son demasiado
próximos para separar varias series consecutivas.

### 14.5. Prefijo común en nombres

Se añadió `%cf` a las plantillas. Calcula el prefijo común de los nombres base y
elimina separadores finales `_`, `-` o espacio.

Ejemplo:

```text
IMG_1201.dng + IMG_1202.dng + IMG_1203.dng → %cf = IMG
```

## 15. Correspondencia GUI/CLI

| Función | GUI | CLI |
|---|:---:|---|
| Alineamiento entero/subpíxel/afín | Sí | `--alignment` |
| Desactivar alineamiento | Sí | `--no-align` |
| Recorte óptimo | Sí | `--no-crop` |
| Nivel blanco personalizado | Sí | `-w` |
| Deghosting | Sí | `--deghost N` |
| Fusión robusta/heredada/desactivada | Sí | `--fusion MODE` |
| Preservar exposición | Sí | `--preserve-exposure` |
| 16/24/32 bits | Sí | `-b` |
| Tamaño de preview DNG | Sí | `-p` |
| Radio de máscara | Sí | `-r` |
| Guardar máscara | Sí | `-m` |
| Procesamiento batch | No | `--batch` |
| Tamaño fijo de bracket | No | `--bracket-size` |
| Importar máscara | Sí | No |
| Edición manual y undo/redo | Sí | No |
| Proyectos JSON | Sí | No |
| Zoom y overlays | Sí | No |

## 16. Compilación y distribución para Windows

### 16.1. GitHub Actions

Existe un workflow específico para la rama experimental:

```text
Build Windows x64 (experimental)
```

Utiliza GitHub Actions con MSYS2 UCRT64 y construye los ejecutables GUI y CLI.
Entre las dependencias instaladas están Qt 5, LibRaw, Exiv2, Zlib, ALGLIB,
OpenMP y OpenCV.

### 16.2. Dependencias transitivas

Después de `windeployqt`, el script inspecciona recursivamente ejecutables y DLL
con `ldd`. Copia cada dependencia situada en el prefijo MinGW hasta alcanzar un
punto fijo. Esto incluye las bibliotecas transitivas de OpenCV y reduce los
fallos que solo aparecen en equipos sin entorno de desarrollo.

### 16.3. Plugin de plataforma Qt

El paquete contiene:

```ini
[Paths]
Plugins = .
```

en `qt.conf`, además de `platforms/qwindows.dll`. Esto hace explícita la ruta de
plugins y evita el error de Windows:

```text
This application failed to start because no Qt platform plugin could be initialized
```

### 16.4. Artefacto

El workflow publica:

```text
HDRMerge-experimental-Windows-x64
```

con retención de siete días. El paquete incorpora los dos ejecutables, plugins
Qt, DLL transitivas, licencia, README y `BUILD-INFO.txt`.

## 17. Pruebas y validación realizadas

### 17.1. Test automatizado CFA

`test/testCfaAlignment.cpp` comprueba:

- que una transformación identidad reproduce exactamente las muestras;
- que un warp con traslación fraccionaria y rotación conserva separadas las
  cuatro fases Bayer;
- que la identidad declara huella de interpolación nula y un warp fraccionario
  declara incertidumbre de remuestreo en la mayor parte de su área válida;
- que no se reduce inesperadamente el área válida.

El test asigna rangos numéricos disjuntos a `R`, `G1`, `G2` y `B`; cualquier
mezcla entre fases provoca un fallo inmediato.

CTest se ejecuta en GitHub Actions después de compilar. La ejecución que valida
la nueva composición y el alineamiento informó:

```text
Test #1: cfa-alignment ... Passed
Test #2: merge-quality ... Passed
100% tests passed
```

### 17.2. Tests de fusión y ruido

`test/testMergeQuality.cpp` cubre las siguientes regresiones sintéticas:

- la fusión adaptativa está activada por defecto;
- la confianza de saturación se desvanece de forma continua antes del recorte;
- un canal CFA no saturado no hunde el umbral blanco común;
- un degradado RAW con una región atípica recupera la razón de exposición sin
  dejar una discontinuidad en el cambio de fuente;
- tres exposiciones con razones `8:2:1` recuperan una escala global coherente;
- una frontera directa entre las capas 0 y 2 comprueba que la capa 1 no aparece
  como consecuencia del feathering;
- una pila con dos muestras coherentes y un valor temporal extremo comprueba el
  rechazo robusto del outlier;
- dos muestras incompatibles comprueban el fallback a la exposición seleccionada
  por la máscara;
- dos muestras próximas comprueban que ambas contribuyen cuando el tamaño
  efectivo de muestra se acerca a dos;
- dos valores brillantes sin consenso comprueban que ya no existe una tolerancia
  arbitraria proporcional a la señal;
- el mismo caso confirma que `legacy` conserva deliberadamente el promedio
  permisivo anterior y produce un resultado distinto de `robust`;
- un punto discrepante aislado y una región discrepante compacta comprueban que
  el primero se trata como ruido y la segunda como movimiento.
- un campo CFA uniforme con ruido Poisson-Gaussiano, tres exposiciones y dos
  transformaciones subpíxel afines comprueba que la confianza de `robust` no
  adquiere una modulación ligada a la rejilla Bayer;
- en ese campo, `robust` debe mantener una confianza media superior a `0,99`,
  una diferencia media entre vecinos de la misma fase inferior a `0,01` y un
  error cuadrático medio no superior al de `legacy` por más de un 5 %;
- el mismo ensayo exige que `robust` reduzca el error cuadrático medio al menos
  un 15 % frente a la composición sin fusión.

En la ejecución de referencia de este ensayo, retirar la falsa varianza de
registro elevó la confianza media de `0,983` a `0,997`, redujo la variación
entre vecinos CFA de `0,026` a `0,0054` y bajó ligeramente el error cuadrático
medio de `19,52` a `19,34`. Estas cifras prueban la corrección del mecanismo
sintético; la desaparición completa del artefacto observado debe confirmarse
todavía con los RAW que lo reproducen.

### 17.3. Prueba real subpíxel

Se fusionaron correctamente los tres DNG Nikon D5000 incluidos en `test/` con:

```bash
hdrmerge-nogui --alignment subpixel -o sample-subpixel.dng \
  sample1.dng sample2.dng sample3.dng
```

El resultado fue un DNG de 16 bits y `4306×2858` píxeles, con código de salida
cero.

### 17.4. Prueba real afín

La misma serie se procesó con `--alignment affine`. Las transformaciones
aceptadas mostraron aproximadamente:

```text
imagen 1: posición (-3,787; -2,144), rotación +0,0098°, confianza 0,999
imagen 0: posición (-1,646; +1,037), rotación -0,0812°, confianza 0,921
```

Se generó un DNG de 16 bits y `4304×2856` píxeles.

### 17.5. Round-trip DNG

El DNG afín generado se volvió a abrir con HDRMerge y se guardó otra vez sin
alineamiento. La operación terminó con código cero, validando al menos la
coherencia estructural del archivo para LibRaw/HDRMerge.

### 17.6. Bracket real Canon

La composición se comprobó también con `IMG_2586.CR2`, `IMG_2587.CR2` e
`IMG_2588.CR2`. La corrección mantuvo el blanco útil en el rango declarado por
la cámara, eliminó el arco cromático que coincidía con el cambio de exposición
y conservó el contorno de la corona sin una transición cromática visible.

En un recorte oscuro de `800 x 800`, muestreando por separado las cuatro fases
CFA, la nueva fusión redujo la desviación estándar aproximadamente entre un 3 %
y un 8 % frente a `--no-average`. La formulación anterior alcanzaba entre un 5 %
y un 11 % en el mismo recorte, pero aceptaba más fácilmente discrepancias en
zonas brillantes. La pérdida moderada de reducción de ruido es deliberada: el
nuevo estimador da prioridad a conservar detalle débil y bordes cuando el
registro o la escala radiométrica no justifican un promedio completo.

El mosaico RAW generado con `--no-average` se comparó con el resultado del modo
de selección anterior y no presentó diferencias de píxel. Esta opción permite
separar en una prueba A/B la contribución de la nueva fusión de los cambios en
alineamiento, respuesta o máscara.

### 17.7. Errores y despliegue

También se comprobó que:

- `--help` expone las nuevas opciones;
- una entrada inexistente devuelve código `1`;
- una entrada inexistente no crea una salida parcial;
- el ejecutable GUI localiza `qt.conf` y `qwindows.dll`;
- el artefacto contiene las DLL de OpenCV necesarias.

La ejecución de CI que valida la revisión funcional `353456a` es:

```text
https://github.com/nogj/hdrmerge/actions/runs/33840818929
```

## 18. Ejemplos de uso

### Subpíxel predeterminado

```bash
hdrmerge-nogui.exe --nogui -o resultado.dng toma_1.dng toma_2.dng toma_3.dng
```

### Afín con diagnóstico

```bash
hdrmerge-nogui.exe --nogui -vv --alignment affine \
  -o resultado.dng toma_1.dng toma_2.dng toma_3.dng
```

### Modo compatible entero

```bash
hdrmerge-nogui.exe --alignment integer -o resultado.dng *.dng
```

### Serie estática con reducción de ruido

```bash
hdrmerge-nogui.exe --deghost 12 -o resultado.dng *.dng
```

La reducción de ruido ya está activa en este ejemplo. Para obtener la
composición basada únicamente en la máscara:

```bash
hdrmerge-nogui.exe --no-average -o resultado.dng *.dng
```

Para conservar el promedio experimental anterior, más intenso:

```bash
hdrmerge-nogui.exe --fusion legacy -o resultado.dng *.dng
```

### Lotes de tres exposiciones para panorama

```bash
hdrmerge-nogui.exe --batch --bracket-size 3 --preserve-exposure \
  -o "%id[0]/%cf-HDR.dng" *.dng
```

## 19. Archivos principales modificados

| Archivo | Responsabilidad |
|---|---|
| `src/CfaAlignment.cpp/.hpp` | ECC, validación geométrica y remuestreo CFA-safe |
| `src/Image.cpp/.hpp` | transformación, validez, escala radiométrica y saturación |
| `src/ImageStack.cpp/.hpp` | respuesta global, máscara, recorte y fusión adaptativa |
| `src/ImageIO.cpp/.hpp` | carga robusta, salida transaccional y máscaras PNG |
| `src/Launcher.cpp/.hpp` | CLI, batch, códigos de salida y selección GUI/CLI |
| `src/LoadOptionsDialog.cpp/.hpp` | modo de alineamiento y deghosting |
| `src/DngPropertiesDialog.cpp/.hpp` | promedio y preservación de exposición |
| `src/MainWindow.cpp/.hpp` | proyectos, máscaras, zoom, estado y errores |
| `src/PreviewWidget.cpp/.hpp` | zoom, vista neutra y avisos de saturación |
| `src/DngFloatWriter.cpp/.hpp` | resultado verificable del escritor |
| `src/ExifTransfer.cpp/.hpp` | propagación de fallos de metadatos |
| `test/testCfaAlignment.cpp` | invariantes del remuestreo Bayer |
| `test/testMergeQuality.cpp` | regresiones de máscara, movimiento y ruido robusto |
| `.github/workflows/windows.yml` | CI y artefacto Windows experimental |
| `scripts/build-windows.sh` | ensamblado portable y dependencias transitivas |

## 20. Limitaciones conocidas

### 20.1. Patrones CFA

El remuestreo subpíxel y afín admite actualmente patrones Bayer repetitivos
`2×2`. X-Trans y disposiciones poco comunes conservan el comportamiento
heredado: si `RawParameters::canAlign()` no considera segura la geometría, el
alineamiento se desactiva.

### 20.2. Modelo global

La transformación es global para toda la imagen. No corrige:

- paralaje entre objetos a distintas distancias;
- movimiento independiente dentro de la escena;
- rolling shutter;
- deformaciones locales complejas;
- cambios ópticos no representables mediante una matriz afín.

### 20.3. Interpolación

El remuestreo CFA utiliza interpolación bilineal dentro de cada fase. Es
conservadora y evita mezcla cromática, pero puede suavizar ligeramente el
detalle y correlacionar espacialmente parte del ruido. La fusión robusta ya no
convierte la fase fraccionaria de esta interpolación en una modulación adicional
de confianza, pero no elimina la correlación inherente al remuestreo. Filtros de
orden superior requerirían control explícito de ringing, valores negativos y
altas luces.

### 20.4. Validación fotográfica

Los tests comprueban invariantes estructurales y una serie RAW real, pero no
sustituyen un conjunto amplio de cámaras, ISOs, ópticas y escenas. Deben
revisarse especialmente:

- estrellas y luces puntuales;
- patrones repetitivos y moiré;
- ramas movidas por viento;
- agua, reflejos y pantallas;
- exposiciones con mucho ruido;
- transformaciones próximas a los límites aceptados.

### 20.5. Linear DNG

No existe todavía una ruta RGB específica para Linear DNG. El diseño actual
está orientado al mosaico RAW CFA.

### 20.6. Memoria

El escritor DNG continúa renderizando la salida completa en memoria. Archivos
muy grandes o pilas numerosas pueden requerir bastante RAM. Una evolución
posterior debería escribir por tiles o bandas.

### 20.7. Proyectos

Los proyectos guardan rutas absolutas y no empaquetan los RAW. No son portables
si cambia la ubicación de los archivos.

### 20.8. Interfaz

La GUI ha recibido mejoras funcionales, pero continúa basada en la organización
clásica de Qt Widgets. El rediseño visual y de flujo de trabajo queda separado
de esta fase para no mezclar una reestructuración de interfaz con la validación
del nuevo motor.

## 21. Siguientes pasos recomendados

1. Construir un corpus de brackets RAW de varias cámaras.
2. Añadir métricas objetivas de error antes/después del alineamiento.
3. Permitir seleccionar la referencia automática, media o manual.
4. Ensayar un refinamiento piramidal ECC para rotaciones mayores.
5. Comparar interpolación bilineal con filtros CFA de mayor calidad.
6. Implementar una ruta específica para X-Trans.
7. Añadir importación de máscaras y proyectos a la CLI.
8. Escribir el DNG por tiles para reducir memoria.
9. Abordar en una fase independiente el rediseño completo de la GUI.
