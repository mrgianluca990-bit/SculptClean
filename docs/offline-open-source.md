# Motore offline Cathar: integrazione, licenze e limiti

Sculpt Clean usa il programma separato **Cathar 0.8.0** per gli interventi di restauro offline. Il codice di Cathar non è inserito nel callback audio del VST3: il lavoro viene eseguito su una copia temporanea del file, poi il risultato può essere ascoltato e salvato. Il costo della renderizzazione rimane reale; questa separazione evita di tenere quei calcoli attivi durante il mix.

## Sorgente effettivamente inclusa

| Voce | Valore |
| --- | --- |
| Progetto originale | <https://github.com/vbasky/cathar> |
| Versione | 0.8.0 |
| Commit bloccato | `f2c2842f89084589d069e5a8a0b61311aa70d928` |
| Licenza | MIT **oppure** Apache-2.0; entrambe conservate in `ThirdParty/Cathar` |
| Archivio incluso | `ThirdParty/Cathar/cathar-0.8.0-source.tar.gz` |
| SHA-256 dell'archivio | `2c6cfa7e876d452d81b32ff8c1515363ec6437d4c72a235bc03af382fd014939` |
| Toolchain di build | Rust 1.90.0; `Cargo.lock` originale con `--locked` |
| Funzionalità Rust abilitate | Nessuna funzionalità opzionale: `--no-default-features` |
| Funzionalità escluse | `ml`, `tui` |
| Modifiche al sorgente originale | Nessuna |

L'archivio consente di compilare il codice individuato anche senza dipendere dal contenuto futuro del ramo principale. Il checksum viene verificato prima dell'estrazione. La prima compilazione scarica ancora la toolchain Rust e le dipendenze Cargo: la sola presenza dell'archivio non rende la prima build completamente priva di rete. Il Mac che esegue l'app compilata non richiede Rust, Python, un server o l'invio dell'audio a servizi esterni.

Le licenze delle dipendenze sono distinte da quella di Cathar. I decoder Symphonia dichiarano MPL-2.0; `ThirdParty/Cathar/DEPENDENCY-LICENSES.txt` contiene le licenze originali dei pacchetti selezionati per entrambe le architetture macOS, e `dependency-sources.json` conserva versione e URL dell'archivio sorgente di ogni dipendenza. Sono incluse anche le attribuzioni della libreria standard Rust. Non si descrive quindi l'intero eseguibile come «solo MIT».

`tools/build-cathar.sh` compila su macOS sia `aarch64-apple-darwin` sia `x86_64-apple-darwin`, unisce i due eseguibili con `lipo`, verifica le due architetture e firma localmente il risultato. La firma locale non equivale a una notarizzazione Apple. `cmake/Cathar.cmake` espone il target di build `SculptCathar` e il percorso `SCULPT_CATHAR_BINARY`; l'app incorpora il programma risultante nelle proprie risorse.

## Operazioni e parametri del motore

La convenzione è `cathar COMANDO INPUT --out OUTPUT`. Il chiamante deve passare una lista di argomenti al processo, senza costruire un comando di shell con nomi di file. Le seguenti sono le operazioni esaminate per questa integrazione; la disponibilità di un comando nell'upstream non significa che tutte le sue opzioni siano esposte nell'interfaccia.

| Operazione | Argomenti esaminati | Significato e limiti |
| --- | --- | --- |
| De-crackle | `decrackle --sensitivity 5` | Rilevazione degli impulsi con seconda differenza, ricostruzione Hermite. Valori da 1 a 10; valori alti intervengono di più. |
| De-click AR | `declick --threshold 10 --method ar` | Interpolazione autoregressiva delle zone impulsive individuate. Alzare la soglia riduce la sensibilità. |
| De-hum adattivo | `dehum --freq 0 --harmonics 8 --adaptive` | Frequenza 0 avvia la scelta automatica fra 50/60 Hz; inseguimento e rimozione delle armoniche. |
| De-ess multibanda | `deesser --freq 4000 --threshold 6 --bands 3` | Controllo delle bande alte rispetto alla loro media recente. Qui la soglia è **+6 dB** sopra la media, non la soglia −24 usata dal diverso metodo a banda singola. |
| Plosive | `deplosive --strength 4 --method events` | Rilevamento degli eventi nelle basse frequenze. La soglia in dB è `clamp(16 − strength, 6, 18)`: valori più alti aumentano la sensibilità. Il valore 4 corrisponde a 12 dB; non equivale al wet/dry. |
| Rustle | `derustle --strength 4` | Attenuazione spettrale degli eventi irregolari, da ascoltare sul materiale reale. |
| Vento/rumble | `dewind --cutoff 80` | Filtro passa-alto; non separa in modo intelligente il vento dal contenuto musicale nella stessa banda. |
| De-reverb WPE | `dereverb --wpe --taps 15 --delay 3` | Predizione lineare ponderata nel dominio STFT, tre iterazioni. `--strength` viene ignorato dal percorso WPE: un controllo quantità deve essere applicato nel chiamante mediante wet/dry. |
| EQ Match | `rebalance --reference RIFERIMENTO --strength 0.5` | Confronto dello spettro medio e correzione EQ, limitata a ±12 dB. Non è separazione in voce, basso, batteria e altri strumenti. |
| Normalizzazione loudness | `normalize --target=-16 --true-peak=-1` | Misura LUFS e applica un guadagno limitato dal ceiling true peak; non equivale a un livellatore dinamico o a un limiter multibanda. |
| Wow/flutter | `dewow` | Stabilizzazione rispetto a una componente tonale dominante; più appropriata quando esiste un riferimento stabile. Può variare leggermente la durata. |
| Ricampionamento | `resample --rate 48000` | Cambia frequenza di campionamento e numero di campioni. |
| Misure | `stats INPUT` | Stampa picco, RMS, LUFS, true peak e altri indicatori; non usa `--out` e non produce JSON. |

Il formato di lavoro verificato è WAV float32; la copia originale va conservata. Per i comandi che cambiano il tempo o il sample rate, il chiamante deve gestire esplicitamente il nuovo numero di campioni invece di trattarli come filtri a durata invariata.

## Cosa non viene presentato come AI o come equivalente a RX

Il modulo upstream `voiceisolate` combina una rilevazione energetica dell'attività vocale con un gate spettrale; non è una rete addestrata alla separazione fra dialogo e musica. `breath` individua zone vicine all'inizio del parlato e applica un trattamento passa-alto: non è un riconoscitore neurale affidabile dei respiri. Questi moduli non vengono usati come prova di una funzione Dialogue Isolate o Breath Control equivalente a RX e non entrano in preset automatici di pulizia.

La funzionalità opzionale `ml` di Cathar è esclusa. L'upstream contiene un modello preaddestrato di circa 2,1 MB su **toni sintetici con rumore**: l'autore specifica che non è calibrato sul parlato. Il diverso costruttore `NeuralDenoiser::new()` inizializza invece un modello sostanzialmente passante, utile come punto di partenza. Non si confondono i due percorsi e non si presenta il modello sintetico come pulizia vocale già verificata. L'integrazione vocale RNNoise, documentata separatamente, usa pesi effettivi per il parlato ed è un motore diverso.

Il comando `rebalance` di Cathar è un EQ Match: il nome non deve essere mostrato come «Music Rebalance» o «estrazione stem». Anche una lunga lista di moduli DSP non prova parità di qualità, ricostruzione o flusso di lavoro con RX.

### Limiti specifici del WPE

Il WPE implementato usa STFT da 1024 campioni e hop da 256. Il ritardo è espresso in frame: tre frame valgono 48 ms a 16 kHz, 16 ms a 48 kHz e 8 ms a 96 kHz. Il codice riceve il sample rate ma non ridimensiona automaticamente queste finestre. Una configurazione non produce quindi lo stesso comportamento fisico a tutte le frequenze di campionamento.

La predizione può attenuare anche note desiderate molto stabili. La verifica inclusa misura intenzionalmente questo caso e lo registra come limite, invece di nasconderlo fra i test favorevoli. È un elaboratore offline da usare con ascolto comparato e quantità prudente, soprattutto sulla musica. I canali vengono elaborati separatamente; la coerenza stereo su ogni registrazione non è dimostrata da un test mono.

### Limiti specifici del wow/flutter

Il rilevatore segue una componente tonale dominante e corregge il tempo con interpolazione lineare. Non è un sistema universale di recupero del trasporto di un nastro: vibrato intenzionale, assenza di riferimento stabile, più strumenti sovrapposti e stereo con contenuto diverso richiedono particolare attenzione. La durata può variare; il risultato non va inserito automaticamente in una selezione senza verificarne l'allineamento.

## Verifica riproducibile

`tools/test-cathar.py` usa soltanto la libreria standard Python e genera file WAV deterministici. Avvia il vero eseguibile, legge il vero audio restituito, verifica valori finiti e conservazione dei file di ingresso. Le fixture misurano hum con armonica, filtro del rumble, impulsi isolati e densi, coda di riverbero simulata, de-ess, trasparenza di EQ Match con riferimento identico, canali, resampling e parsing dei comandi.

```sh
python3 tools/test-cathar.py \
  --binary build-cathar/bin/cathar \
  --report docs/cathar-linux-verification.json
```

Il report riporta il commit originale, l'hash dell'eseguibile esaminato, le misure, i tempi di elaborazione delle fixture e i limiti rilevati. I tempi sono riferiti al computer di prova e a file sintetici di pochi secondi; non sono una promessa di prestazioni su un Mac o su una sessione lunga. I test upstream a confronto binario dipendono dalla piattaforma su cui sono stati prodotti; non si impone l'identità byte per byte fra FFT arm64 e x86_64 come sostituto di una verifica audio.

### Esito reale su Linux x86_64

La build release dell'eseguibile è riuscita con il sorgente e la toolchain indicati. Le otto fixture di integrazione hanno superato le proprie condizioni. Sono stati eseguiti anche i test unitari originali della libreria con `cargo +1.90.0 test --locked --release --package cathar --lib`: **128 passati, 0 falliti, 1 ignorato dall'upstream** (`spade_trace`, una traccia di debug da avviare manualmente). Il log è in `cathar-upstream-unit-tests.log`. Il file `cathar-linux-verification.json` conserva i valori completi delle fixture; i principali sono:

| Fixture | Misura osservata |
| --- | --- |
| Hum 50 Hz con tono utile 997 Hz | Hum −52,27 dB; tono utile −0,00029 dB |
| Rumble 30 Hz, cutoff 80 Hz | Rumble −34,08 dB; tono 997 Hz praticamente invariato |
| De-click AR | Errore rispetto al segnale pulito ridotto di 91,79 dB |
| De-crackle | Errore rispetto al segnale pulito ridotto di 19,83 dB |
| WPE, coda sintetica | Coda tardiva ridotta di 2,69 dB |
| WPE, nota pulita 440 Hz | **Nota desiderata attenuata di 23,53 dB**: limite rilevante, non risultato favorevole |
| De-ess multibanda | Burst 6 kHz −2,06 dB; tono 311 Hz praticamente invariato |
| EQ Match con riferimento identico | Errore RMS rispetto al segnale −138,88 dB |
| EQ Match con diverso rapporto fra due toni | Rapporto richiesto 10,45757 dB; rapporto ottenuto 10,45757 dB |
| Normalizzazione stereo a −23 LUFS | Misura finale −23,0 LUFS; true peak −20,3 dBTP, sotto il ceiling impostato |
| Opzioni e formati | WAV float32 finito, conservazione dei canali, resampling 16→48 kHz e lettura statistiche verificati |

Queste misure non attestano il risultato su voce, lavalier, concerti o materiale d'archivio reali. La compilazione universale macOS e l'ascolto in DAW restano verifiche distinte: il test Linux non viene presentato come prova che siano già stati eseguiti.

## Separazione musicale e alternative esaminate

**NARA-WPE** (<https://github.com/fgnt/nara_wpe>, MIT) offre una implementazione di riferimento Python/NumPy del WPE. È stato esaminato, ma non aggiunto al runtime: in questa versione il WPE reale proviene dal helper nativo Cathar. Questo evita un secondo ambiente Python soltanto per lo stesso tipo di intervento.

**Demucs** (<https://github.com/adefossez/demucs>) fornisce una vera separazione musicale; la versione corrente esaminata è 4.1.0, commit `829d21890023800713e5dd231c67e43ecc06069b`. Il codice è MIT, ma il README di quel commit chiarisce che la licenza dei pesi preaddestrati non è definita in modo uniforme e che alcuni dati di addestramento hanno vincoli di ricerca/non commerciali. I pesi non vengono quindi redistribuiti come se fossero automaticamente coperti dalla licenza del codice. Non viene dichiarata una separazione funzionante senza installare, verificare ed eseguire un modello reale. Il costo CPU/RAM di PyTorch rimarrebbe inoltre rilevante durante il render, anche se non sarebbe presente dopo il consolidamento delle tracce.

**Open-Unmix UMX-HQ è integrato** nel worker locale di Sculpt Clean Studio per separare voce, batteria, basso e altri strumenti e regolarne poi il livello. Il codice Open-Unmix 1.3.0 è MIT; i quattro checkpoint UMX-HQ scelti hanno una dichiarazione MIT esplicita nel [record ufficiale Zenodo 3370489](https://zenodo.org/records/3370489), verificata anche mediante la sua metadata API. I checkpoint sono stati scaricati, verificati con MD5 e SHA-256 e usati in test di inferenza reale Linux. Il setup dell'app scarica gli stessi pesi verificati e il runtime privato; i pesi non sono contenuti nell'archivio dell'app. La documentazione completa è in `docs/stems-open-source.md`.

La distinzione fra modelli è essenziale: **UMX-HQ è quello scelto; UMX-L/UMXL non è incluso** e indica CC BY-NC-SA 4.0. Il vincolo non commerciale di UMX-L non deve essere attribuito ai diversi pesi UMX-HQ. La separazione UMX-HQ lavora offline e richiede CPU/RAM durante il render; non diventa un effetto neurale leggerissimo in tempo reale e non dimostra parità qualitativa con RX.

**Spleeter** (<https://github.com/deezer/spleeter>) è stato esaminato ma non integrato: non si assume che la licenza MIT del codice risolva da sola tutte le condizioni dei pesi preaddestrati. Anche Demucs rimane un'alternativa esaminata, non un secondo backend installato da questa versione.

Per confrontare la copertura effettiva con il prodotto di riferimento, la lista primaria da controllare è quella di iZotope RX: <https://www.izotope.com/products/rx-standard> e <https://www.izotope.com/products/rx-advanced>. Una voce simile nell'interfaccia non prova equivalenza di algoritmo, risultati o maturità.
