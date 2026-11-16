# Restauro offline: De-clip e Repair

## Codice open source effettivamente riutilizzato

Il motore `Source/OfflineRepair.h` contiene un port C++17 dell'algoritmo
A-SPADE di [Andry Rafaralahy, spade-declipping](https://github.com/andryr/spade-declipping),
licenza MIT, fissato al commit
`d9285bc5d6874670c462773f4c0dba55c6d7f66b`.

Sono stati portati il ciclo iterativo di `tight_aspade.m`, il threshold dei
coefficienti di `hard_thresh.m`, il framing di `declip.m` e la normalizzazione
FFT unitaria. I sei file di riferimento e la licenza sono inclusi in
`ThirdParty/Repair/spade-declipping`; `PROVENANCE.json` registra i blob Git,
confrontati con i byte archiviati. Non è soltanto un riferimento bibliografico.

Il port sostituisce MATLAB/LTFAT con una FFT C++ locale e non richiede download,
Python, modelli neurali o connessioni di rete per questi due trattamenti.
La teoria del metodo è descritta anche in
[S-SPADE Done Right](https://arxiv.org/abs/1809.09847).

Sono stati esaminati anche Audacity Clip Fix/Repair e la toolbox
`rajmic/declipping2020_codes`. Clip Fix usa interpolazione dei picchi ed è
documentato per clipping lieve; la toolbox di ricerca richiede MATLAB e, per
alcuni metodi, LTFAT/CVX. Per questa integrazione è stato scelto il piccolo
nucleo MIT portabile. Non sono stati copiati il codice GPL di Audacity o quello
GPLv3 della toolbox del survey.

## De-clip

Il rilevatore cerca plateau quasi perfettamente piatti vicino ai picchi
positivi e negativi. La soglia automatica viene stimata separatamente per
ciascun canale; si può specificare una soglia lineare assoluta. Per impostazione
iniziale occorrono almeno tre campioni consecutivi: un singolo impulso non
viene trattato come clipping.

Per ogni finestra interessata il solver alterna una rappresentazione spettrale
sparsa alla proiezione sui vincoli della registrazione. I campioni affidabili
restano fissati all'originale; i campioni tagliati vengono ricostruiti oltre il
rispettivo limite di clipping. Le finestre sono ricomposte con overlap-add e
normalizzazione. Le finestre senza clipping saltano il solver iterativo.

Il controllo Amount miscela la correzione soltanto sui campioni rilevati. Il
limite iniziale di ricostruzione è 12 dB oltre il rail stimato. Non viene
applicata normalizzazione automatica a tutto il file: i picchi ricostruiti
possono superare 0 dBFS, quindi prima di esportare in PCM intero serve verificare
il livello. L'export float conserva i picchi senza tagliarli.

Il rilevatore è conservativo per evitare di riscrivere normali creste musicali.
Questa versione tratta **hard clipping con plateau**, non promette di riconoscere
clipping morbido, limiter, MP3 o clipping alterato da rumore/dithering successivi.
Un'onda volutamente squadrata può essere indistinguibile da un segnale tagliato.
Le parti già mancanti non sono recuperabili in modo univoco: il metodo propone
una ricostruzione coerente con i campioni rimasti e con l'ipotesi spettrale sparsa.

## Repair di una selezione

La stessa ottimizzazione viene adattata a una selezione danneggiata: i campioni
al suo interno possono variare liberamente, mentre il contesto resta fissato.
Questo permette di ricostruire click e brevi segmenti, oltre il limite di uno o
due campioni del De-click in insert.

Il solver usa il contesto ai due lati e accetta al massimo 4096 campioni per
operazione, ampliando la FFT quando necessario. Quel massimo è un limite
operativo, non una promessa qualitativa per buchi di 85 ms. I test favorevoli
qui sotto riguardano selezioni di 16–128 campioni; per materiale reale conviene
selezionare solo il difetto e confrontare il risultato. Ai bordi del file,
senza almeno 16 campioni integri per lato, l'operazione viene rifiutata.

Non è una ricostruzione generativa del parlato: non può inventare in modo
affidabile consonanti o parole perse. Rumore largo di banda e attacchi unici
possono adattarsi poco all'ipotesi di sparsità.

## CPU e annullamento

Sono funzioni **offline**, da eseguire su un worker mentre il VST3 continua ad
avere il proprio motore leggero. Le iterazioni FFT non devono essere chiamate
dal callback audio. Consumano CPU durante il render e non aggiungono un carico
continuo dopo aver applicato la correzione al file.

Tutti i canali vengono completati prima di modificare i buffer del chiamante.
Annullamento e richieste non valide lasciano il materiale originale invariato;
sono esposti flag atomico di cancellazione e progresso. La stima del picco
prima/dopo e i conteggi di campioni/regioni sono restituiti al chiamante.

## Verifiche riproducibili

Test: `Tests/offline_repair_test.cpp`, C++17 con
`-O2 -Wall -Wextra -Wpedantic -Werror -pthread`.

| Fixture con originale noto | Miglioramento dell'errore quadratico |
|---|---:|
| Somma armonica, hard clip a ±0,38 | 13,42 dB |
| Somma di frequenze modulata in ampiezza, hard clip a ±0,24 | 21,28 dB |
| Selezione danneggiata di 16 campioni | 20,53 dB |
| Selezione danneggiata di 64 campioni | 16,28 dB |
| Selezione danneggiata di 128 campioni | 23,49 dB |

I valori misurano la differenza rispetto al segnale sintetico pulito noto,
non una valutazione percettiva su registrazioni reali. Il materiale armonico
favorisce il modello sparso; non si inferisce lo stesso risultato su qualunque
registrazione o equivalenza con RX.

Passano anche: neutralità sui toni non clippati a 20/110/997/4001 Hz, Amount
zero, conservazione esatta dei campioni affidabili e di tutto ciò che è fuori
selezione, antiphase stereo (errore massimo 0), impulso breve voluto intatto,
limiti della selezione, input non finiti e cancellazione prima/durante il render
senza modifiche parziali.

AddressSanitizer e UndefinedBehaviorSanitizer hanno superato gli stessi test.
LeakSanitizer era disattivato (`detect_leaks=0`) per il limite dell'ambiente
sotto ptrace; non viene dichiarato come eseguito. È verificato anche che una
soglia manuale troppo bassa, con boost massimo a zero, non abbassi i picchi
tagliati violando il vincolo di consistenza.

Una misura indicativa locale Linux del primo fixture (0,25 s mono) ha richiesto
circa 0,33 s. È un render offline, non una misura del consumo VST3 in una DAW
né una certificazione di prestazioni su macOS.

## API

`sculptclean::OfflineRepair::declip(...)` elabora l'intero buffer passato;
`repairRegion(...)` modifica soltanto la selezione. Entrambe accettano puntatori
ai canali, numero di canali/campioni, sample rate, opzioni, cancellazione e
progresso opzionali. `Report` espone `ok`, `cancelled`, `changed`, `regions`,
`samples`, `peakBefore`, `peakAfter` e un messaggio per l'interfaccia.
