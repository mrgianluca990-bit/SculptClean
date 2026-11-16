# Sculpt Clean 0.2.0

Suite di restauro per macOS, mono e stereo, composta da **Sculpt Clean Studio**
per lavorare sui file e **Sculpt Clean VST3** per gli insert nella DAW. Lo Studio
aggiunge editor con forma d'onda e spettrogramma, 27 operazioni selezionabili,
riparazione locale, denoise neurale e separazione musicale in quattro stem.

**Sono forniti sorgenti e compilazione automatica. La versione macOS non è
stata eseguita in questo ambiente e non è dimostrata una parità con RX12.**
Le elaborazioni nuove sono implementate e provate su Linux con i motori reali;
restano necessarie build nativa, prova in DAW e ascolto su registrazioni reali.
I risultati e i limiti misurati sono in [docs/VERIFICA.md](docs/VERIFICA.md).

## Flusso di lavoro

1. Apri una registrazione in **Sculpt Clean Studio**. Il documento rimane in
   memoria; il file originale sul disco viene conservato.
2. Trascina sulla forma d'onda per scegliere un intervallo, oppure sullo
   spettrogramma per scegliere tempo e frequenze. Usa **Zoom selezione** per
   i difetti piccoli. Il rettangolo in frequenza controlla il modulo
   **Attenua selezione spettrale**; gli altri moduli usano l'intervallo temporale.
3. Scegli il trattamento e premi **Elabora selezione**. Per Denoise con profilo,
   seleziona prima almeno 2,1 secondi di solo rumore e premi **Apprendi rumore**.
   Voice Denoise RNNoise non richiede questo apprendimento.
4. Controlla il risultato con **Ascolta**, **Prima** e **Rimosso**. Prima e
   Rimosso confrontano il documento corrente con il file aperto inizialmente,
   quindi includono l'effetto cumulativo degli interventi. Sono disponibili
   **Annulla**, **Ripeti** e **Ripristina selezione** dall'originale.
5. Esporta un nuovo **WAV float32**, alla frequenza originale oppure a
   44,1/48/96 kHz, e importalo nella DAW. L'esportazione comprende l'intero
   documento. Usa il VST3 per le regolazioni leggere che vuoi mantenere in insert.

Il render lavora in background e può essere interrotto. Una correzione riuscita
viene aggiunta alla cronologia; l'originale non viene riscritto. L'export scrive
prima un file temporaneo e completa la sostituzione soltanto a scrittura riuscita;
un WAV di destinazione esistente richiede la conferma del selettore di salvataggio.
La cronologia è limitata e non è un formato di progetto persistente: esporta il
risultato prima di chiudere. L'app chiede conferma prima di abbandonare modifiche
non esportate. File lunghi e separazione in stem richiedono RAM.

Float32 conserva anche valori superiori a 0 dBFS generati da De-clip o dal
rebalance. Non è un limiter: controlla i livelli e normalizza prima di una
successiva conversione in PCM intero o dell'invio a un'uscita audio.

## Operazioni dello Studio

| # | Operazione | Implementazione e limite principale |
|---:|---|---|
| 1 | Denoise — profilo rumore | Sottrazione/attenuazione spettrale con profilo fisso appreso dal rumore selezionato. |
| 2 | Voice Denoise — RNNoise | Rete compatta inclusa, pensata per il parlato; conversione offline a 48 kHz e ritorno alla frequenza originale. |
| 3 | Dereverb — leggero | Modello della potenza delle code, condiviso con il VST3; può accorciare decadimenti musicali voluti. |
| 4 | Dereverb — WPE offline | Predizione spettrale tramite Cathar; miscela iniziale 20%, uso preferibile sul parlato. |
| 5 | De-clip — SPADE | Ricostruzione iterativa dei plateau tagliati; soglia automatica o manuale. Non ripara ogni forma di distorsione. |
| 6 | De-click — predittivo | Rilevazione di impulsi e interpolazione autoregressiva. Non è un riconoscitore specifico di rumori della bocca. |
| 7 | De-crackle | Rilevazione di impulsi densi e ricostruzione Hermite, con sensibilità regolabile. |
| 8 | De-hum — automatico | Scelta 50/60 Hz e inseguimento del ronzio con otto armoniche. |
| 9 | De-ess — multibanda | Attenuazione degli eccessi nelle bande alte; frequenza iniziale regolabile. |
| 10 | De-plosive | Rilevazione e attenuazione di eventi nelle basse frequenze. |
| 11 | De-wind / low cut | Filtro passa-alto regolabile; coinvolge anche il segnale utile nella stessa banda. |
| 12 | De-rustle | Attenuazione spettrale di eventi irregolari; richiede controllo sul materiale reale. |
| 13 | Attenua selezione spettrale | Riduzione nel rettangolo tempo/frequenza, con bordi sfumati. Non risintetizza il contenuto cancellato. |
| 14 | Ripara intervallo breve | Ricostruzione da contesto con solver sparso; massimo 4096 campioni, senza garanzia su buchi lunghi. |
| 15 | Source Extractor | Riduzione dinamica sotto soglia derivata da Sculpt Strip; non separa sorgenti sovrapposte. |
| 16 | EQ Match da riferimento | Correzione dello spettro medio verso quello di un altro file. È distinta dal rebalance degli stem. |
| 17 | Loudness LUFS | Normalizzazione al target, limitata dal ceiling di −1 dBTP; il target può non essere raggiunto se manca headroom. |
| 18 | Normalizza picco | Guadagno verso il picco in dBFS scelto. |
| 19 | Gain | Guadagno in dB sulla selezione. |
| 20 | Rimuovi DC | Sottrazione della media della selezione per canale. |
| 21 | Fade in | Dissolvenza lineare d'ingresso sulla selezione. |
| 22 | Fade out | Dissolvenza lineare d'uscita sulla selezione. |
| 23 | Inverti polarità | Cambio di segno dei campioni selezionati. |
| 24 | Scambia L/R | Scambio dei canali stereo. |
| 25 | Wow / Flutter — tono guida | Correzione temporale dell'intero file usando una componente tonale dominante; può cambiare la durata. |
| 26 | Centro stereo (M) | Somma Mid dei due canali; coinvolge tutti gli elementi al centro. |
| 27 | Rimuovi centro stereo | Differenza Side dei canali; non equivale all'estrazione neurale di una voce. |

Intensità regola la quantità o la miscela dei trattamenti; a zero l'audio rimane
invariato. Per le utility 17–24 e 26–27 il trattamento viene applicato al 100%,
così target di livello, polarità e scambio canali hanno il significato indicato.
Wow/Flutter usa l'intero documento al 100%: non inserisce un risultato di durata
diversa dentro una selezione. Dopo una variazione di durata, Prima, Rimosso e
Ripristina selezione vengono disabilitati perché i campioni non corrispondono
più nel tempo; Annulla recupera la versione precedente.

### Limiti da ascoltare

WPE può togliere anche note stabili desiderate: un test a trattamento completo
ha attenuato un tono pulito di 23,53 dB. La miscela iniziale al 20% riduce
l'intervento ma non garantisce la conservazione di ogni strumento. Anche il
dereverb leggero può accorciare code asciutte. RNNoise può alterare musica e
immagine stereo; la sua destinazione preferita è una voce mono.

De-clip è conservativo e riconosce soprattutto hard clipping con plateau.
Repair richiede contesto integro su entrambi i lati: il limite di 4096 campioni
non significa poter recuperare una parola persa. Wow/Flutter richiede un tono
guida stabile; vibrato voluto e musica complessa possono indurre correzioni errate.

## Separazione in quattro stem e rebalance

**Prepara separazione** installa una volta l'ambiente privato e i quattro modelli
**Open-Unmix UMX-HQ**. Il primo setup richiede Internet e spazio per Python,
PyTorch e dipendenze, oltre ai circa 142,6 MB dei pesi. Non richiede Homebrew o
privilegi amministrativi. RNNoise, SPADE e il resto dello Studio non richiedono
questo setup opzionale.

**Separa 4 stem** elabora l'intero documento e crea una nuova cartella con
`vocals.wav`, `drums.wav`, `bass.wav`, `other.wav` e `stems.json`. Regola quindi
**Voce**, **Batteria**, **Basso** e **Altro** e premi **Applica rebalance**.
−60 dB equivale a mute; doppio clic riporta il guadagno a 0 dB. Per ascoltare
solo la voce lascia Voce a 0 dB e silenzia gli altri tre gruppi.

Il processo usa CPU locale e al massimo due thread; l'audio non viene inviato
a un servizio remoto. La separazione neurale consuma comunque tempo e memoria
durante il render. Non separa arbitrariamente ogni strumento o ogni dialogo.
I residui di ricostruzione confluiscono nello stem Altro, così i quattro stem
a 0 dB ricompongono il mix entro l'arrotondamento float. Dettagli, licenze e
test sono in [docs/stems-open-source.md](docs/stems-open-source.md).

## VST3 e consumo CPU

Il VST3 conserva il motore della 0.1: **De-hum → De-click → Denoise/Dereverb →
Source → Output**, con bypass allineato e ascolto del rimosso. De-click in
insert riguarda soprattutto impulsi di uno o due campioni; il De-click
predittivo e De-crackle più estesi sono nello Studio. De-hum del VST3 usa
notch fissi a 50/60 Hz; quello adattivo è nello Studio.

Denoise parte a 35% ma resta inattivo senza profilo; Dereverb, De-click e
De-hum partono a zero, Source OFF e Output 0 dB. Apprendi circa due secondi
di solo rumore durante la riproduzione. Il profilo viene salvato nel progetto
DAW e viene invalidato cambiando frequenza di campionamento.

La latenza fissa del VST3 è **2051 campioni**, anche in bypass: 46,51 ms a
44,1 kHz, 42,73 ms a 48 kHz e 21,36 ms a 96 kHz. La DAW deve compensarla;
per monitoraggio live rimane Sculpt Live Clean. Lo Studio compensa le latenze
dei propri motori prima di reinserire i campioni renderizzati.

Il plug-in usa una sola STFT da 2048 punti, hop 512, condivisa fra Denoise e
Dereverb; su Mac usa Accelerate/vDSP. Il motore audio non alloca buffer C++
durante il processo e sospende le FFT quando non servono. **RNNoise, SPADE,
WPE e Open-Unmix lavorano nello Studio e non aggiungono inferenza continua agli
insert.** Il bypass mantiene attivi i moduli per un confronto coerente:
disattiva o congela il plug-in nella DAW se vuoi liberarne il carico.

La misura precedente di una singola istanza completa era circa 3,6–3,7% del
budget audio su Linux; 16 istanze sincronizzate superavano il budget in alcuni
callback. È documentata come misura del core VST3, **non** come benchmark della
nuova suite o certificazione della CPU su Mac. L'elaborazione offline evita
un costo permanente dopo il render; non rende gratuiti i calcoli più complessi.

## Compilazione su GitHub e installazione

Il file di avvio è incluso, con il punto iniziale di `.github`:

```text
.github/workflows/build-macos-vst3.yml
```

Carica nella radice del repository il **contenuto** di `SculptClean`: devono
esserci `CMakeLists.txt`, `Source`, `Studio`, `Tests`, `tools`, `cmake`,
`ThirdParty`, `docs` e `.github`. Se il browser omette la cartella nascosta,
usa **Add file → Create new file** e inserisci il percorso completo del workflow.
Con il caricamento dal browser puoi caricare le cartelle in più passaggi;
non caricare lo ZIP come unico file del repository.
La compilazione parte al push ed è avviabile anche da **Actions → Run workflow**.

Scarica l'artifact **Sculpt-Clean-macOS**. Il pacchetto **Sculpt-Clean-macOS.zip**
contiene `Sculpt Clean.vst3` e `Sculpt Clean Studio.app`, compilati per Apple
Silicon e Intel. L'artifact **Sculpt-Clean-verifiche** conserva log, risultati
e immagine prodotta dal test nativo della UI.
JUCE e Cathar vengono compilati da sorgente; la prima build scarica le dipendenze
e richiede più tempo di un semplice aggiornamento del plug-in. Gli script
fissano versioni e sorgenti.

Per compilare sul proprio Mac servono Xcode Command Line Tools, CMake 3.22 o
successivo, Python 3 e Rust con `cargo` e `rustup`. Lo script installa la
toolchain Rust 1.90.0 e i due target macOS, se mancanti. Questi strumenti sono
requisiti della compilazione, non dell'uso dell'app già compilata:

```bash
bash tools/test-core.sh
bash tools/test-studio.sh
bash tools/build-macos.sh
```

`SCULPT_JUCE_SOURCE_DIR` può indicare una copia locale di JUCE 8.0.4.
Il risultato della build è in `dist/Sculpt-Clean-macOS.zip`.
Copia l'app in Applicazioni e il bundle VST3 in:

```text
~/Library/Audio/Plug-Ins/VST3/Sculpt Clean.vst3
```

Riapri la DAW o aggiorna la scansione VST3. I bundle hanno firma locale ad hoc,
non notarizzazione Apple. Le verifiche macOS del workflow e il successivo
caricamento sulle DAW di destinazione restano necessari.

## Codice open source riutilizzato e parti ancora mancanti

| Componente reale | Uso | Provenienza e condizioni |
|---|---|---|
| RNNoise v0.1 | Voice Denoise | Sorgenti C e modello compilato inclusi, licenza BSD; [dettagli](docs/voice-open-source.md). |
| A-SPADE | De-clip e Repair | Port C++ del nucleo MIT `andryr/spade-declipping`, originali e revisione archiviati; [dettagli](docs/repair-open-source.md). |
| Cathar 0.8.0 | Motori offline e misure | Sorgente fissata, MIT oppure Apache-2.0; alcune dipendenze, fra cui Symphonia, sono MPL-2.0. Licenze separate incluse; [dettagli](docs/offline-open-source.md). |
| Open-Unmix 1.3.0 + UMX-HQ | Quattro stem e rebalance | Codice MIT e pesi UMX-HQ dichiarati MIT dal record ufficiale Zenodo. UMX-L non viene usato; [dettagli](docs/stems-open-source.md). |
| JUCE 8.0.4 | Interfaccia, audio e formato VST3 | Moduli sotto AGPLv3 oppure licenza commerciale JUCE; testo originale in `ThirdParty/JUCE-8.0.4-LICENSE.md`. Le condizioni dei diversi componenti rimangono distinte. |

Queste aggiunte coprono numerosi interventi assenti nella 0.1. Rimangono
funzioni e verifiche importanti per l'obiettivo RX: separazione avanzata di
scene/dialogo/musica/effetti, navigazione e modifica attraverso la trascrizione,
ricostruzione spettrale estesa e risintesi, riconoscimento specializzato di
mouth click/respiri, ricostruzione dell'ambiente, workflow completo a più
documenti e valutazione percettiva su archivi reali. Lo spettrogramma e
l'attenuazione selettiva presenti non sostituiscono tutte quelle funzioni.

La quantità di moduli, una licenza aperta o un test sintetico favorevole non
dimostrano la qualità di un prodotto maturo. Per confrontare con RX servono
gli stessi file, livelli allineati, riduzione simile e ascolto di consonanti,
attacchi, code, residui e stereo. Non sono stati eseguiti confronti con RX12
e non è incluso codice o un modello proprietario iZotope.
