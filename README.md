# dlss5-gfn-overlay

Applica **DLSS 5 Neural Rendering in tempo reale** alla finestra di GeForce NOW
(o a qualunque altra finestra), con un tasto per accendere e spegnere l'effetto
e confrontare al volo.

Non è affiliato a NVIDIA, ReShade o RenoDX.

## Cosa fa, e cosa non fa

Il pass di Neural Rendering di DLSS 5 (`nvngx_dlssnr.dll`, feature 18) lavora
**sul colore**: prende un frame e ne restituisce uno migliorato, con controlli
di tono locale, struttura e struttura della pelle. Non gli servono depth buffer
né motion vector veri, ed è per questo che si può applicare a un video già
decodificato. Il progetto [dlss5-visual-enhancer][enhancer] lo fa su file; qui
lo stesso pass viene applicato a una finestra viva, frame per frame.

Quello che **non** succede: nessun DLSS Super Resolution e nessun Frame
Generation sul gioco. Quelli hanno bisogno dei buffer del motore, che sul tuo PC
non esistono perché il gioco gira sui server NVIDIA. Se vuoi il DLSS vero su
GeForce NOW, lo attivi dentro le impostazioni grafiche del gioco: su GFN
Ultimate gira lato server sulle macchine RTX 5080.

Questa app serve per tutto il resto: i giochi che DLSS 5 non ce l'hanno, e in
generale qualunque finestra tu voglia migliorare.

## Come è fatto

```
finestra sorgente
   │  Windows Graphics Capture (zero copie, resta sulla GPU)
   ▼
texture BGRA ──► pass shader: crop + BGRA→RGBA ──► render target RGBA8
                                                        │
                                                        ▼  readback su CPU
                                              worker DLSS-NR (processo separato)
                                              stdin:  header + RGBA + motion
                                              stdout: header + RGBA migliorato
                                                        │
                                                        ▼  upload su GPU
                                              swapchain borderless a schermo intero
```

Tre thread: la callback di cattura tiene solo l'ultimo frame e butta i
precedenti, un thread neurale fa il giro completo attraverso il worker, il
thread principale presenta. **I frame vecchi si scartano invece di accodarli**:
su uno stream cloud la latenza conta più della fluidità.

Il worker gira in un processo separato di proposito. Quando la feature 18
esplode con `0xC0000005` (succede su certe combinazioni driver/GPU) muore il
worker, non l'overlay, e il thread neurale lo fa ripartire da solo.

## Il conto della latenza, prima che tu ci perda tempo

È la cosa che decide se questo progetto ha senso sul tuo hardware, quindi
mettiamola in chiaro subito.

A 1920×1080 ogni frame attraversa le pipe così:

| direzione | dati | byte |
| --- | --- | ---: |
| verso il worker | RGBA8 | 8,3 MB |
| verso il worker | campo motion float16 | 8,3 MB |
| dal worker | RGBA8 migliorato | 8,3 MB |

Sono ~25 MB per frame, cioè **1,5 GB/s a 60 fps**, più il viaggio
GPU→CPU→GPU. A 1440p sono 44 MB per frame. Il campo motion è la voce più
stupida della lista: lo riempiamo di zeri, come fa il percorso immagini del
worker, e lo mandiamo comunque perché il protocollo lo prevede.

Non ti do una stima inventata di quanti millisecondi costa sul tuo PC. C'è
`--bench` apposta:

```
dlss5-gfn-overlay.exe --bench 300
```

Processa 300 frame e stampa mediana e 95° percentile di ogni stadio, più la
latenza vera end-to-end dal momento della cattura a quello della presentazione.
Se il totale supera i 16,6 ms non tieni i 60 fps, e il primo intervento da fare
è sostituire le pipe con memoria condivisa (vedi *Prossimi passi*).

Sopra tutto questo c'è già la latenza dello stream cloud, 30-60 ms. Su un
single player si convive, su un competitivo no.

## Requisiti

- Windows 10 1903 o successivo (Windows 11 per togliere il bordo giallo della
  cattura), 64 bit, Direct3D 11.
- GPU NVIDIA RTX. Ada e Blackwell (RTX 40/50) sono i bersagli principali del
  runtime.
- I file di runtime NVIDIA, ReShade e RenoDX, che **non sono in questo repo**.
  Vedi [`runtime/BINARIES.md`](runtime/BINARIES.md).

### Su RTX 30 (Ampere)

Il Neural Rendering su Ampere gira sul percorso sperimentale ed è nettamente
più lento che su RTX 40/50. Funziona, ma il budget per frame è il problema, e
la configurazione di default quasi certamente non tiene i 60 fps.

Parti da qui invece che dai default:

```ini
neural_input_scale = 0.667
upscale_factor = 1.5
```

Il runtime lavora su 1280x720 e restituisce 1920x1080: meno della metà dei
pixel da elaborare rispetto al DLAA nativo, e altrettanti byte in meno da far
passare nelle pipe. Poi misura con `--bench 300` e alza o abbassa
`neural_input_scale` finchè il totale non sta sotto i 16,6 ms.

Frame generation su Ampere non è disponibile: serve Ada o Blackwell.

## Compilare

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Servono solo Windows SDK e MSVC: niente vcpkg, niente dipendenze esterne. Le
intestazioni C++/WinRT arrivano dall'SDK.

## Usare

1. Metti i file di runtime come descritto in `runtime/BINARIES.md`.
2. Copia `config.example.ini` in `config.ini` accanto all'eseguibile e aggiusta
   `window_title`. Per vedere i titoli disponibili: `--list-windows`.
3. Avvia GeForce NOW, poi l'overlay. Se lo avvii prima, aspetta la finestra.
4. `alt+F1` accende e spegne il Neural Rendering, `alt+F2` stampa le latenze,
   `alt+F4` esce.

In passthrough l'overlay presenta il frame catturato direttamente dalla GPU,
senza giro sul worker: il confronto con l'effetto acceso è quindi onesto, stesso
percorso di presentazione, stessa scala.

Opzioni utili:

```
--window "GeForce NOW"   sovrascrive il titolo dal config
--bench 300              misura e esce
--stats 5                stampa le latenze ogni 5 secondi
--log-level debug        più dettagli, compreso lo stderr del worker
```

## Stato: scritto, non ancora compilato

Il codice è completo e coerente ma **non è mai stato compilato né eseguito**:
è stato scritto su Linux, e serve una macchina Windows con RTX per la prima
build. Aspettati errori di compilazione alla prima passata. Il protocollo del
worker invece non è indovinato: è stato letto dai sorgenti di
[dlss5-visual-enhancer][enhancer] e i `static_assert` sulle dimensioni delle
struct falliscono a compile time se il layout non torna.

Quello che resta da verificare sull'hardware vero:

- che il worker regga una sessione continua con `frame_count = 0` invece di un
  conteggio noto. Se rifiuta il setup, `frame_count_hint` nel config accetta un
  numero grande.
- quanto costa davvero un frame. È la domanda a cui risponde `--bench`.
- se il crop di default va bene per il client GFN, o se serve tagliare della
  chrome.

## Rischi da conoscere

**Termini di servizio.** L'overlay cattura la finestra dall'esterno con
Windows Graphics Capture, non inietta niente nel client GeForce NOW. È più
prudente dell'injection, ma resta un'area grigia: valutala tu.

**Anti-cheat.** Questa app non tocca il processo del gioco. Vale comunque la
regola generale: non usare mod di questo tipo con giochi online competitivi.

**Contenuto protetto.** Se una finestra usa una superficie protetta da DRM, la
cattura restituisce nero. È un limite di Windows, non un bug qui.

## Prossimi passi, in ordine di resa

1. **Abbassare `neural_input_scale`.** È la leva più forte e l'unica che
   agisce su tutti i costi insieme: meno pixel da elaborare, meno byte da
   leggere dalla GPU, meno byte nelle pipe. Con `neural_input_scale = 0.667` e
   `upscale_factor = 1.5` il runtime lavora su 720p e restituisce 1080p.
2. **Sovrapporre cattura e round trip.** Oggi il thread neurale fa cattura,
   readback e giro sul worker in sequenza. Facendo il readback del frame
   successivo mentre il worker lavora sul precedente si nascondono i 2-5 ms
   della cattura dietro i millisecondi del pass neurale. Guadagno reale ma
   modesto: vale la pena solo dopo che `--bench` dice che il resto è a posto.
3. **Frame generation** con `nvngx_dlssg.dll`. Non è disponibile su Ampere:
   serve Ada o Blackwell. Aggiunge comunque un frame di buffer, quindi su cloud
   gaming va misurato prima di deciderlo.

Due ottimizzazioni che sembrano ovvie e non sono possibili: **memoria condivisa
al posto delle pipe** e **non mandare il campo motion quando è tutto zero**.
Entrambe richiedono di cambiare il protocollo del worker, e del worker esiste
solo il binario: i sorgenti nativi del feeder non sono pubblicati. Restano sul
tavolo solo se scrivi un feeder tuo.

## Licenza

Codice originale sotto MIT. La licenza copre solo questo codice: non
ridistribuisce né concede diritti su runtime NVIDIA, RenoDX, ReShade o altri
binari di terze parti. NVIDIA, GeForce RTX, NGX e DLSS sono marchi di NVIDIA
Corporation.

[enhancer]: https://github.com/Merserk/dlss5-visual-enhancer
