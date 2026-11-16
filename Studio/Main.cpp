#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "../Source/OfflineTools.h"
#include "../Source/OfflineRepair.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <iostream>

using sculptclean::OfflineAudio;
using sculptclean::OfflineSelection;
namespace {
const juce::Colour background(0xff101213),panel(0xff1b1e20),ink(0xffece8dc),accent(0xffdfaa62),muted(0xff98a4a5);

std::shared_ptr<OfflineAudio> readAudio(const juce::File& file) {
    juce::AudioFormatManager formats;formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if(!reader)throw std::runtime_error("Formato audio non leggibile");
    if(reader->numChannels<1||reader->numChannels>2)throw std::runtime_error("Apri un file mono o stereo");
    if(reader->lengthInSamples<=0||reader->lengthInSamples>std::numeric_limits<int>::max())throw std::runtime_error("Durata non supportata");
    const double bytes=static_cast<double>(reader->lengthInSamples)*reader->numChannels*sizeof(float);
    const double allowed=std::max(256.0,juce::SystemStats::getMemorySizeInMegabytes()*.18)*1024*1024;
    if(bytes>allowed)throw std::runtime_error("File troppo grande per la memoria disponibile: apri una sezione esportata dalla DAW");
    auto audio=std::make_shared<OfflineAudio>();audio->rate=reader->sampleRate;
    audio->channels.assign(reader->numChannels,std::vector<float>(static_cast<std::size_t>(reader->lengthInSamples)));
    float* pointers[2]{audio->channels[0].data(),reader->numChannels>1?audio->channels[1].data():nullptr};
    if(!reader->read(pointers,static_cast<int>(reader->numChannels),0,static_cast<int>(reader->lengthInSamples)))throw std::runtime_error("Lettura audio incompleta");
    for(auto& channel:audio->channels)for(auto& sample:channel)if(!std::isfinite(sample))sample=0;
    sculptclean::offlineRequire(*audio);return audio;
}
void writeAudio(const juce::File& file,const OfflineAudio& audio) {
    sculptclean::offlineRequire(audio);
    auto stream=file.createOutputStream();if(!stream)throw std::runtime_error("Impossibile scrivere il file WAV");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(),audio.rate,static_cast<unsigned>(audio.channels.size()),32,{},0));
    if(!writer)throw std::runtime_error("Impossibile creare WAV float32");
    (void)stream.release();
    const float* channels[2]{audio.channels[0].data(),audio.channels.size()>1?audio.channels[1].data():nullptr};
    if(!writer->writeFromFloatArrays(channels,static_cast<int>(audio.channels.size()),static_cast<int>(audio.size())))throw std::runtime_error("Scrittura WAV incompleta");
}
juce::Image spectrumImage(const OfflineAudio& audio,const std::atomic<bool>* cancel) {
    constexpr int width=1200,height=320,fftSize=1024;
    juce::Image image(juce::Image::RGB,width,height,true);juce::dsp::FFT fft(10);
    std::array<float,fftSize*2> buffer{};std::array<float,fftSize/2+1> power{};
    constexpr double pi=3.14159265358979323846;
    juce::Image::BitmapData pixels(image,juce::Image::BitmapData::writeOnly);
    for(int x=0;x<width;++x){
        if(x%32==0)sculptclean::offlineCheckCancel(cancel);
        power.fill(0);const auto centre=static_cast<std::int64_t>(static_cast<double>(x)/width*audio.size());
        for(const auto& channel:audio.channels){
            buffer.fill(0);for(int n=0;n<fftSize;++n){const auto index=centre+n-fftSize/2;if(index>=0&&index<static_cast<std::int64_t>(audio.size()))buffer[static_cast<std::size_t>(n)]=channel[static_cast<std::size_t>(index)]*static_cast<float>(.5-.5*std::cos(2*pi*n/fftSize));}
            fft.performFrequencyOnlyForwardTransform(buffer.data());
            for(int n=0;n<=fftSize/2;++n)power[static_cast<std::size_t>(n)]+=buffer[static_cast<std::size_t>(n)]*buffer[static_cast<std::size_t>(n)];
        }
        for(int y=0;y<height;++y){
            const double hz=20*std::pow(audio.rate*.5/20,1.0-static_cast<double>(y)/(height-1));
            const int bin=juce::jlimit(0,fftSize/2,static_cast<int>(hz*fftSize/audio.rate));
            const double db=10*std::log10(std::max(1e-14,static_cast<double>(power[static_cast<std::size_t>(bin)])/(audio.channels.size()*fftSize*fftSize*.0625)));
            const float v=static_cast<float>(std::clamp((db+95)/85,0.0,1.0));
            pixels.setPixelColour(x,y,juce::Colour::fromHSV(.70f-.60f*v,.70f,.04f+.96f*v,1));
        }
    }
    return image;
}
juce::File catharBinary() {
    const auto app=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto bundled=app.getParentDirectory().getSiblingFile("Resources").getChildFile("cathar");
    if(bundled.existsAsFile())return bundled;
    const auto development=app.getSiblingFile("cathar");if(development.existsAsFile())return development;
    throw std::runtime_error("Motore offline Cathar assente: usa il pacchetto completo prodotto dalla compilazione");
}
juce::File stemsResources(){return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getSiblingFile("Resources").getChildFile("stems");}
juce::File stemsInstallation(){return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Application Support/SculptClean/stems");}
struct TemporaryFolder {
    juce::File folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sculpt-clean-render","",false);
    TemporaryFolder(){if(!folder.createDirectory())throw std::runtime_error("Impossibile creare la cartella temporanea");}
    ~TemporaryFolder(){folder.deleteRecursively();}
};
juce::String childCommand(const juce::StringArray& arguments,const std::atomic<bool>& cancel,
                          const juce::File& cancelFile=juce::File{}) {
    juce::ChildProcess process;
    if(!process.start(arguments))throw std::runtime_error("Impossibile avviare il motore offline");
    std::string output;
    std::thread reader([&]{std::array<char,4096> bytes{};for(;;){const int count=process.readProcessOutput(bytes.data(),static_cast<int>(bytes.size()));if(count<=0)break;if(output.size()<256000)output.append(bytes.data(),static_cast<std::size_t>(count));}});
    const auto start=std::chrono::steady_clock::now();auto interruption=start;bool interrupted=false;
    while(process.isRunning()){
        const auto now=std::chrono::steady_clock::now();
        if(!interrupted&&(cancel.load()||now-start>std::chrono::minutes(30))){
            interrupted=true;interruption=now;
            if(cancelFile==juce::File{}||!cancelFile.replaceWithText("cancel")){process.kill();break;}
        }
        if(interrupted&&now-interruption>std::chrono::seconds(45)){process.kill();break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    process.waitForProcessToFinish(10000);reader.join();
    if(interrupted)throw std::runtime_error(cancel.load()?"Operazione annullata":"Tempo massimo di elaborazione superato");
    const auto text=juce::String::fromUTF8(output.data(),static_cast<int>(output.size()));
    if(process.getExitCode()!=0)throw std::runtime_error(("Motore offline: "+text.substring(0,2000)).toStdString());
    return text;
}

class PreviewSource final:public juce::PositionableAudioSource {
public:
    PreviewSource(std::shared_ptr<const OfflineAudio> a,std::shared_ptr<const OfflineAudio> before,
                  OfflineSelection range,bool original,bool removed)
        :audio(std::move(a)),baseline(std::move(before)),selection(range),useOriginal(original),useRemoved(removed){}
    void prepareToPlay(int,double)override{}
    void releaseResources()override{}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info)override{
        info.clearActiveBufferRegion();const auto& input=useOriginal?*baseline:*audio;
        for(int n=0;n<info.numSamples&&position+n<getTotalLength();++n){
            const auto index=selection.start+static_cast<std::size_t>(position+n);
            for(int c=0;c<info.buffer->getNumChannels();++c){const auto channel=std::min(static_cast<std::size_t>(c),input.channels.size()-1);float sample=input.channels[channel][index];if(useRemoved)sample=baseline->channels[channel][index]-audio->channels[channel][index];info.buffer->setSample(c,info.startSample+n,sample);}
        }
        position+=info.numSamples;
    }
    void setNextReadPosition(juce::int64 value)override{position=std::clamp<juce::int64>(value,0,getTotalLength());}
    juce::int64 getNextReadPosition()const override{return position;}
    juce::int64 getTotalLength()const override{return static_cast<juce::int64>(selection.end-selection.start);}
    bool isLooping()const override{return false;}
private:
    std::shared_ptr<const OfflineAudio> audio,baseline;OfflineSelection selection;
    juce::int64 position=0;bool useOriginal=false,useRemoved=false;
};

class Studio;
class AudioCanvas final:public juce::Component {
public:
    std::shared_ptr<OfflineAudio> audio;juce::Image spectrum;OfflineSelection selection;
    std::function<void(OfflineSelection)> onSelection;
    double viewStart=0,viewEnd=1,playhead=-1;
    void paint(juce::Graphics& g)override {
        g.fillAll(panel);if(!audio){g.setColour(muted);g.setFont(juce::Font(juce::FontOptions(18)));g.drawText("Apri una registrazione per iniziare",getLocalBounds(),juce::Justification::centred);return;}
        const auto area=getLocalBounds().toFloat().reduced(12);const float waveHeight=140;
        const auto wave=area.withHeight(waveHeight);const auto spec=area.withTop(wave.getBottom()+18).withBottom(area.getBottom()-24);
        const double first=viewStart*audio->size(),last=viewEnd*audio->size(),span=std::max(1.0,last-first);
        g.setColour(juce::Colour(0xff77c4bc));
        for(int x=0;x<static_cast<int>(wave.getWidth());++x){
            const auto a=static_cast<std::size_t>(first+x/wave.getWidth()*span),b=std::min(audio->size(),static_cast<std::size_t>(first+(x+1)/wave.getWidth()*span)+1);float peak=0;
            const auto step=std::max<std::size_t>(1,(b-a)/24);for(auto n=a;n<b;n+=step)for(const auto& channel:audio->channels)peak=std::max(peak,std::abs(channel[n]));
            const float extent=std::min(1.0f,peak)*waveHeight*.46f;g.drawVerticalLine(static_cast<int>(wave.getX())+x,wave.getCentreY()-extent,wave.getCentreY()+extent);
        }
        if(spectrum.isValid())g.drawImage(spectrum,static_cast<int>(spec.getX()),static_cast<int>(spec.getY()),static_cast<int>(spec.getWidth()),static_cast<int>(spec.getHeight()),static_cast<int>(viewStart*spectrum.getWidth()),0,std::max(1,static_cast<int>((viewEnd-viewStart)*spectrum.getWidth())),spectrum.getHeight());
        const auto timeX=[&](std::size_t n){return area.getX()+static_cast<float>((n-first)/span)*area.getWidth();};
        const float x0=juce::jlimit(area.getX(),area.getRight(),timeX(selection.start)),x1=juce::jlimit(area.getX(),area.getRight(),timeX(selection.end));
        const float y0=frequencyY(selection.highHz,spec),y1=frequencyY(selection.lowHz,spec);
        g.setColour(accent.withAlpha(.13f));g.fillRect(x0,wave.getY(),x1-x0,wave.getHeight());
        g.setColour(accent);g.drawRect(juce::Rectangle<float>(x0,y0,std::max(1.0f,x1-x0),std::max(1.0f,y1-y0)),1.3f);
        if(playhead>=0){const float x=timeX(static_cast<std::size_t>(playhead*audio->rate));g.setColour(ink);g.drawVerticalLine(static_cast<int>(x),area.getY(),spec.getBottom());}
        g.setColour(muted);g.setFont(juce::Font(juce::FontOptions(11)));
        g.drawText(juce::String(first/audio->rate,2)+" s",static_cast<int>(area.getX()),static_cast<int>(area.getBottom()-18),100,18,juce::Justification::centredLeft);
        g.drawText(juce::String(last/audio->rate,2)+" s",static_cast<int>(area.getRight()-100),static_cast<int>(area.getBottom()-18),100,18,juce::Justification::centredRight);
        g.drawText("Spettro: 20 Hz — "+juce::String(audio->rate*.0005,1)+" kHz",static_cast<int>(area.getCentreX()-160),static_cast<int>(area.getBottom()-18),320,18,juce::Justification::centred);
    }
    void mouseDown(const juce::MouseEvent& e)override {if(!audio)return;dragStart=e.position;dragging=true;select(e.position);}
    void mouseDrag(const juce::MouseEvent& e)override {if(dragging)select(e.position);}
    void mouseUp(const juce::MouseEvent&)override {dragging=false;}
private:
    float frequencyY(double hz,juce::Rectangle<float> area)const {const double f=std::clamp(hz,20.0,audio->rate*.5);return area.getBottom()-static_cast<float>(std::log(f/20)/std::log(audio->rate*.5/20))*area.getHeight();}
    void select(juce::Point<float> p){
        const auto area=getLocalBounds().toFloat().reduced(12);const auto spec=area.withTop(area.getY()+158).withBottom(area.getBottom()-24);
        const auto sample=[&](float x){const double t=viewStart+juce::jlimit(0.0f,1.0f,(x-area.getX())/area.getWidth())*(viewEnd-viewStart);return std::min(audio->size(),static_cast<std::size_t>(t*audio->size()));};
        selection.start=std::min(sample(dragStart.x),sample(p.x));selection.end=std::max(selection.start+1,std::max(sample(dragStart.x),sample(p.x)));selection.end=std::min(selection.end,audio->size());
        if(dragStart.y>=spec.getY()){
            const auto hz=[&](float y){const float v=juce::jlimit(0.0f,1.0f,(spec.getBottom()-y)/spec.getHeight());return 20*std::pow(audio->rate*.5/20,v);};selection.lowHz=std::min(hz(dragStart.y),hz(p.y));selection.highHz=std::max(hz(dragStart.y),hz(p.y));
        }else{selection.lowHz=0;selection.highHz=audio->rate*.5;}
        if(onSelection)onSelection(selection);
        repaint();
    }
    juce::Point<float> dragStart;bool dragging=false;
};

class Studio final:public juce::AudioAppComponent,private juce::Timer {
public:
    explicit Studio(bool initialiseAudio=true):progressBar(progressDisplay){
        setOpaque(true);setSize(1240,820);
        look.setColour(juce::ResizableWindow::backgroundColourId,background);look.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff303538));look.setColour(juce::TextButton::textColourOffId,ink);look.setColour(juce::ComboBox::backgroundColourId,panel);look.setColour(juce::ComboBox::textColourId,ink);look.setColour(juce::PopupMenu::backgroundColourId,panel);look.setColour(juce::PopupMenu::textColourId,ink);setLookAndFeel(&look);
        for(auto* b:{&open,&save,&undo,&redo,&play,&stop,&original,&removed,&apply,&learn,&cancel,&all,&zoom,&unzoom,&restore,&reference,&statistics,&audioSettings,&setupStems,&separateStems,&rebalanceStems,&revealStems})addAndMakeVisible(*b);
        for(auto* c:std::initializer_list<juce::Component*>{&module,&amount,&parameter,&canvas,&progressBar,&status,&help,&amountLabel,&parameterLabel,&selectionLabel,&exportRate})addAndMakeVisible(*c);
        const char* names[]{"Denoise — profilo rumore","Voice Denoise — RNNoise","Dereverb — leggero","Dereverb — WPE offline","De-clip — SPADE","De-click — predittivo","De-crackle","De-hum — automatico","De-ess — multibanda","De-plosive","De-wind / low cut","De-rustle","Attenua selezione spettrale","Ripara intervallo breve","Source Extractor","EQ Match da riferimento","Loudness LUFS","Normalizza picco","Gain","Rimuovi DC","Fade in","Fade out","Inverti polarità","Scambia L/R","Wow / Flutter — tono guida","Centro stereo (M)","Rimuovi centro stereo"};
        for(int i=0;i<27;++i)module.addItem(names[i],i+1);
        module.onChange=[this]{configureModule();};module.setSelectedId(1);
        for(auto* slider:{&amount,&parameter}){slider->setSliderStyle(juce::Slider::LinearHorizontal);slider->setTextBoxStyle(juce::Slider::TextBoxBelow,false,100,24);slider->setColour(juce::Slider::thumbColourId,accent);}
        amount.setRange(0,100,1);amount.setValue(60);amount.setTextValueSuffix(" %");amountLabel.setText("Intensità / miscela",juce::dontSendNotification);
        const std::array<const char*,4> stemNames{"Voce","Batteria","Basso","Altro"};
        for(std::size_t i=0;i<stemGains.size();++i){auto& slider=stemGains[i];slider.setSliderStyle(juce::Slider::LinearHorizontal);slider.setTextBoxStyle(juce::Slider::TextBoxBelow,false,80,22);slider.setRange(-60,12,.1);slider.setValue(0);slider.setTextValueSuffix(" dB");slider.setDoubleClickReturnValue(true,0);slider.setColour(juce::Slider::thumbColourId,accent);slider.setTooltip("−60 dB silenzia lo stem. Doppio clic: 0 dB.");addAndMakeVisible(slider);stemLabels[i].setText(stemNames[i],juce::dontSendNotification);stemLabels[i].setColour(juce::Label::textColourId,ink);addAndMakeVisible(stemLabels[i]);}
        for(auto* label:{&status,&help,&amountLabel,&parameterLabel,&selectionLabel})label->setColour(juce::Label::textColourId,ink);
        help.setJustificationType(juce::Justification::topLeft);help.setFont(juce::Font(juce::FontOptions(13)));status.setText("Apri un file mono o stereo. Gli originali restano sul disco; esporta il risultato in un nuovo WAV.",juce::dontSendNotification);
        original.setClickingTogglesState(true);removed.setClickingTogglesState(true);
        original.onClick=[this]{removed.setToggleState(false,juce::dontSendNotification);stopPreview();};removed.onClick=[this]{original.setToggleState(false,juce::dontSendNotification);stopPreview();};
        open.onClick=[this]{chooseOpen();};save.onClick=[this]{chooseSave();};undo.onClick=[this]{undoRedo(false);};redo.onClick=[this]{undoRedo(true);};play.onClick=[this]{startPreview();};stop.onClick=[this]{stopPreview();};apply.onClick=[this]{render();};learn.onClick=[this]{learnProfile();};cancel.onClick=[this]{cancelled=true;};
        all.onClick=[this]{if(current){selection={0,current->size(),0,current->rate*.5};updateSelection();}};
        zoom.onClick=[this]{if(current){canvas.viewStart=static_cast<double>(selection.start)/current->size();canvas.viewEnd=static_cast<double>(selection.end)/current->size();canvas.repaint();}};
        unzoom.onClick=[this]{canvas.viewStart=0;canvas.viewEnd=1;canvas.repaint();};
        restore.onClick=[this]{restoreSelection();};reference.onClick=[this]{chooseReference();};statistics.onClick=[this]{showStatistics();};
        setupStems.onClick=[this]{installStems();};separateStems.onClick=[this]{chooseStemFolder();};rebalanceStems.onClick=[this]{renderStemMix();};revealStems.onClick=[this]{if(stemFolder.isDirectory())stemFolder.revealToUser();};
        setupStems.setTooltip("Prepara una volta Python e il modello Open-Unmix locali. Richiede connessione per il download; l'audio non viene inviato in rete.");
        separateStems.setTooltip("Separa l'intero documento in voce cantata, batteria, basso e altro. Elaborazione offline; massimo 2 thread.");
        audioSettings.onClick=[this]{auto* selector=new juce::AudioDeviceSelectorComponent(deviceManager,0,0,0,2,false,false,true,false);selector->setSize(520,360);juce::DialogWindow::LaunchOptions options;options.content.setOwned(selector);options.dialogTitle="Uscita audio";options.dialogBackgroundColour=background;options.escapeKeyTriggersCloseButton=true;options.useNativeTitleBar=true;options.resizable=false;options.launchAsync();};
        canvas.onSelection=[this](OfflineSelection value){selection=value;updateSelection();};
        exportRate.addItem("Export: frequenza originale",1);exportRate.addItem("Export: 44,1 kHz",2);exportRate.addItem("Export: 48 kHz",3);exportRate.addItem("Export: 96 kHz",4);exportRate.setSelectedId(1);
        progressBar.setPercentageDisplay(true);if(initialiseAudio)setAudioChannels(0,2);startTimerHz(20);configureModule();updateButtons();
    }
    ~Studio()override {cancelled=true;stopPreview();if(worker.joinable())worker.join();shutdownAudio();setLookAndFeel(nullptr);}
    void prepareToPlay(int size,double rate)override{transport.prepareToPlay(size,rate);}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& block)override{transport.getNextAudioBlock(block);}
    void releaseResources()override{transport.releaseResources();}
    void requestClose(std::function<void()> close){
        if(busy){setStatus("Interrompi l'elaborazione prima di chiudere l'app.");return;}
        confirmDiscard(std::move(close));
    }
    void loadSelfTestAudio(std::shared_ptr<OfflineAudio> audio){
        current=initial=savedSnapshot=std::move(audio);canvas.audio=current;
        canvas.spectrum=spectrumImage(*current,nullptr);selection={0,current->size(),0,current->rate*.5};
        updateSelection();updateButtons();setStatus("Verifica locale · audio sintetico");
    }
    void paint(juce::Graphics& g)override{
        g.fillAll(background);g.setColour(ink);g.setFont(juce::Font(juce::FontOptions(22)));g.drawText("SCULPT CLEAN",20,8,250,34,juce::Justification::centredLeft);
        g.setColour(muted);g.setFont(juce::Font(juce::FontOptions(11)));g.drawText("STUDIO   ·   0.2.0   ·   RESTAURO OFFLINE",290,12,430,28,juce::Justification::centredLeft);
        g.setColour(panel);g.fillRoundedRectangle(12,90,260,static_cast<float>(getHeight()-168),8);
    }
    void resized()override{
        int x=20;for(auto* b:{&open,&save,&undo,&redo,&play,&stop,&original,&removed}){b->setBounds(x,48,b==&original||b==&removed?110:85,30);x+=b==&original||b==&removed?118:93;}
        audioSettings.setBounds(getWidth()-138,12,118,28);
        module.setBounds(24,108,236,34);amountLabel.setBounds(24,157,232,22);amount.setBounds(24,181,232,55);parameterLabel.setBounds(24,251,232,22);parameter.setBounds(24,276,232,55);
        apply.setBounds(24,347,236,38);learn.setBounds(24,397,236,30);reference.setBounds(24,439,236,30);restore.setBounds(24,481,236,30);statistics.setBounds(24,523,236,30);help.setBounds(24,570,236,std::max(90,getHeight()-660));
        canvas.setBounds(286,90,getWidth()-304,getHeight()-356);selectionLabel.setBounds(292,getHeight()-258,getWidth()-310,24);
        setupStems.setBounds(290,getHeight()-226,160,28);separateStems.setBounds(460,getHeight()-226,160,28);rebalanceStems.setBounds(630,getHeight()-226,160,28);revealStems.setBounds(800,getHeight()-226,130,28);
        const int stemWidth=(getWidth()-314)/4;
        for(std::size_t i=0;i<stemGains.size();++i){const int sx=290+static_cast<int>(i)*stemWidth;stemLabels[i].setBounds(sx,getHeight()-194,stemWidth-10,18);stemGains[i].setBounds(sx,getHeight()-176,stemWidth-10,50);}
        all.setBounds(290,getHeight()-112,130,28);zoom.setBounds(432,getHeight()-112,142,28);unzoom.setBounds(586,getHeight()-112,118,28);exportRate.setBounds(getWidth()-288,getHeight()-112,270,28);
        status.setBounds(16,getHeight()-70,getWidth()-142,30);cancel.setBounds(getWidth()-120,getHeight()-68,102,28);progressBar.setBounds(20,getHeight()-28,getWidth()-40,12);
    }
private:
    void timerCallback()override{
        progressDisplay=busy?static_cast<double>(progress.load()):0;
        if(transport.isPlaying()){canvas.playhead=previewStart+transport.getCurrentPosition();canvas.repaint();}
        else if(canvas.playhead>=0){canvas.playhead=-1;canvas.repaint();}
    }
    bool originalAligned()const{return current&&initial&&current->size()==initial->size()&&current->rate==initial->rate;}
    void updateButtons(){for(auto* b:{&open,&save,&undo,&redo,&play,&apply,&learn,&all,&zoom,&unzoom,&restore,&reference,&statistics})b->setEnabled(!busy&&(b==&open||b==&reference||current!=nullptr));cancel.setEnabled(busy);undo.setEnabled(!busy&&!undoStack.empty());redo.setEnabled(!busy&&!redoStack.empty());module.setEnabled(!busy);setupStems.setEnabled(!busy);separateStems.setEnabled(!busy&&current!=nullptr);rebalanceStems.setEnabled(!busy&&current&&current==stemSource.lock()&&stemFolder.isDirectory());revealStems.setEnabled(stemFolder.isDirectory());for(auto* b:{&original,&removed,&restore})b->setEnabled(!busy&&originalAligned());if(!originalAligned()){original.setToggleState(false,juce::dontSendNotification);removed.setToggleState(false,juce::dontSendNotification);}}
    void updateSelection(){canvas.selection=selection;canvas.repaint();if(current)selectionLabel.setText("Selezione  "+juce::String(selection.start/current->rate,3)+" — "+juce::String(selection.end/current->rate,3)+" s     |     "+juce::String(selection.lowHz,0)+" — "+juce::String(selection.highHz,0)+" Hz",juce::dontSendNotification);}
    void setStatus(const juce::String& message){status.setText(message,juce::dontSendNotification);}
    void stopPreview(){transport.stop();transport.setSource(nullptr);memorySource.reset();canvas.playhead=-1;canvas.repaint();}
    void startPreview(){
        if(!current||busy)return;
        stopPreview();
        const auto count=selection.end-selection.start;if(count==0)return;
        memorySource=std::make_unique<PreviewSource>(current,initial,selection,original.getToggleState(),removed.getToggleState());
        transport.setSource(memorySource.get(),0,nullptr,current->rate);previewStart=selection.start/current->rate;transport.start();
    }
    template<class Work,class Done> void launch(const juce::String& name,Work work,Done done){
        if(busy)return;
        stopPreview();if(worker.joinable())worker.join();busy=true;cancelled=false;progress=-1;setStatus(name);updateButtons();
        juce::Component::SafePointer<Studio> safe(this);
        worker=std::thread([this,safe,work=std::move(work),done=std::move(done)]()mutable{
            try{auto result=work();sculptclean::offlineCheckCancel(&cancelled);juce::MessageManager::callAsync([safe,result=std::move(result),done=std::move(done)]()mutable{if(auto* self=safe.getComponent()){self->busy=false;self->progress=1;done(*self,std::move(result));self->updateButtons();}});}
            catch(const std::exception& error){const auto message=juce::String::fromUTF8(error.what());juce::MessageManager::callAsync([safe,message]{if(auto* self=safe.getComponent()){self->busy=false;self->setStatus(message);self->updateButtons();}});}
        });
    }
    struct RenderResult {std::shared_ptr<OfflineAudio> audio;juce::Image image;juce::String note;};
    RenderResult picture(std::shared_ptr<OfflineAudio> audio,const juce::String& note){return{audio,spectrumImage(*audio,&cancelled),note};}
    void commit(RenderResult result){
        if(current){undoStack.push_back(current);std::size_t total=0;for(auto it=undoStack.rbegin();it!=undoStack.rend();++it)total+=(*it)->size()*(*it)->channels.size()*sizeof(float);while(undoStack.size()>1&&(undoStack.size()>8||total>512ULL*1024*1024)){total-=undoStack.front()->size()*undoStack.front()->channels.size()*sizeof(float);undoStack.erase(undoStack.begin());}}
        const bool durationChanged=current&&current->size()!=result.audio->size();
        redoStack.clear();current=std::move(result.audio);canvas.audio=current;canvas.spectrum=std::move(result.image);
        if(durationChanged){selection={0,current->size(),0,current->rate*.5};canvas.viewStart=0;canvas.viewEnd=1;}
        setStatus(result.note);updateSelection();
    }
    void confirmDiscard(std::function<void()> proceed){
        if(!current||current==savedSnapshot){proceed();return;}
        juce::Component::SafePointer<Studio> safe(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,"Modifiche non esportate",
            "Le modifiche sono ancora in memoria. Esportale in WAV per conservarle, oppure prosegui senza salvarle.",
            "Prosegui senza salvare","Torna al documento",this,
            juce::ModalCallbackFunction::create([safe,proceed=std::move(proceed)](int result){if(safe&&result==1)proceed();}));
    }
    void chooseOpen(){juce::Component::SafePointer<Studio> safe(this);confirmDiscard([safe]{if(auto* self=safe.getComponent())self->chooseOpenFile();});}
    void chooseOpenFile(){
        chooser=std::make_unique<juce::FileChooser>("Apri audio",juce::File{},"*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");juce::Component::SafePointer<Studio> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& dialog){if(auto* self=safe.getComponent()){const auto file=dialog.getResult();if(!file.existsAsFile())return;self->launch("Apertura e analisi dello spettro…",[self,file]{return self->picture(readAudio(file),file.getFileName());},[file](Studio& s,RenderResult result){s.sourceFile=file;s.initial=result.audio;s.current=result.audio;s.savedSnapshot=result.audio;s.stemFolder=juce::File{};s.stemSource.reset();s.undoStack.clear();s.redoStack.clear();s.noiseProfile={};s.canvas.audio=s.current;s.canvas.spectrum=result.image;s.canvas.viewStart=0;s.canvas.viewEnd=1;s.selection={0,s.current->size(),0,s.current->rate*.5};s.setStatus(result.note+" · "+juce::String(s.current->rate,0)+" Hz · "+juce::String(static_cast<int>(s.current->channels.size()))+" canali");s.updateSelection();});}});
    }
    void chooseSave(){
        if(!current)return;
        chooser=std::make_unique<juce::FileChooser>("Esporta nuovo WAV float32",sourceFile.getSiblingFile(sourceFile.getFileNameWithoutExtension()+"-clean.wav"),"*.wav");juce::Component::SafePointer<Studio> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting,[safe](const juce::FileChooser& dialog){if(auto* self=safe.getComponent()){auto destination=dialog.getResult();if(destination==juce::File{})return;const auto chosen=destination;destination=destination.withFileExtension("wav");if(destination!=chosen&&destination.existsAsFile()){self->setStatus("Il WAV finale esiste già: scegli direttamente quel nome e conferma la sostituzione, oppure usa un nuovo nome");return;}if(destination==self->sourceFile){self->setStatus("Scegli un nome diverso: il file originale viene conservato");return;}const auto audio=self->current;const int choice=self->exportRate.getSelectedId();const double rate=choice==2?44100:(choice==3?48000:(choice==4?96000:audio->rate));self->launch("Esportazione WAV float32…",[self,audio,destination,rate]{auto result=sculptclean::offlineResample(*audio,rate,&self->cancelled);juce::TemporaryFile temporary(destination);writeAudio(temporary.getFile(),result);sculptclean::offlineCheckCancel(&self->cancelled);if(!temporary.overwriteTargetFileWithTemporary())throw std::runtime_error("Impossibile completare l'esportazione");return destination;},[audio](Studio& s,juce::File file){s.savedSnapshot=audio;s.setStatus("Esportato: "+file.getFullPathName());});}});
    }
    void chooseReference(){chooser=std::make_unique<juce::FileChooser>("Riferimento per EQ Match",juce::File{},"*.wav;*.aif;*.aiff;*.flac");juce::Component::SafePointer<Studio> safe(this);chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& dialog){if(auto* self=safe.getComponent()){if(dialog.getResult().existsAsFile()){self->referenceFile=dialog.getResult();self->setStatus("Riferimento EQ: "+self->referenceFile.getFileName());}}});}
    void undoRedo(bool forward){auto& from=forward?redoStack:undoStack;auto& to=forward?undoStack:redoStack;if(busy||from.empty())return;auto target=from.back();launch("Ripristino versione…",[this,target]{return picture(target,"Versione ripristinata");},[forward](Studio& s,RenderResult result){auto& src=forward?s.redoStack:s.undoStack;auto& dst=forward?s.undoStack:s.redoStack;dst.push_back(s.current);src.pop_back();const bool durationChanged=s.current->size()!=result.audio->size();s.current=result.audio;s.canvas.audio=s.current;s.canvas.spectrum=result.image;if(durationChanged){s.selection={0,s.current->size(),0,s.current->rate*.5};s.canvas.viewStart=0;s.canvas.viewEnd=1;}s.setStatus(result.note);s.updateSelection();});(void)to;}
    void learnProfile(){if(!current)return;const auto audio=current;const auto range=selection;launch("Apprendimento del rumore selezionato…",[this,audio,range]{return sculptclean::offlineLearn(*audio,range,&cancelled);},[](Studio& s,sculptclean::LearnedState value){s.noiseProfile=value;s.setStatus("Profilo pronto. Seleziona il passaggio da pulire e applica Denoise.");});}
    void restoreSelection(){if(!originalAligned())return;const auto audio=current,base=initial;const auto range=selection;launch("Ripristino dall'originale…",[this,audio,base,range]{auto result=std::make_shared<OfflineAudio>(*audio);for(std::size_t c=0;c<result->channels.size();++c)std::copy(base->channels[c].begin()+static_cast<std::ptrdiff_t>(range.start),base->channels[c].begin()+static_cast<std::ptrdiff_t>(range.end),result->channels[c].begin()+static_cast<std::ptrdiff_t>(range.start));return picture(result,"Selezione ripristinata dall'originale");},[](Studio& s,RenderResult result){s.commit(std::move(result));});}
    void showStatistics(){if(!current)return;const auto audio=current;launch("Analisi livelli…",[this,audio]{TemporaryFolder temporary;const auto input=temporary.folder.getChildFile("input.wav");writeAudio(input,*audio);return childCommand({catharBinary().getFullPathName(),"stats",input.getFullPathName()},cancelled);},[](Studio& s,juce::String result){s.setStatus("Analisi completata");juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,"Statistiche audio",result.substring(0,16000));});}
    void installStems(){
        const auto script=stemsResources().getChildFile("setup-stems.sh");if(!script.existsAsFile()){setStatus("Componenti Stems mancanti nel pacchetto dell'app");return;}
        launch("Preparazione locale di Python e Open-Unmix: download iniziale, senza inviare audio…",[this,script]{TemporaryFolder temporary;const auto cancelFile=temporary.folder.getChildFile("cancel");return childCommand({"/bin/bash",script.getFullPathName(),stemsInstallation().getFullPathName(),cancelFile.getFullPathName()},cancelled,cancelFile);},[](Studio& s,juce::String){s.setStatus("Separazione pronta. Apri un file e scegli Separa 4 stem.");});
    }
    juce::StringArray stemCommand(const juce::String& command){
        const auto python=stemsInstallation().getChildFile("venv/bin/python3"),script=stemsResources().getChildFile("sculpt-stems.py");
        if(!python.existsAsFile()||!script.existsAsFile())throw std::runtime_error("Premi prima Prepara separazione per installare i componenti locali");
        return{python.getFullPathName(),script.getFullPathName(),command};
    }
    void chooseStemFolder(){
        if(!current)return;
        chooser=std::make_unique<juce::FileChooser>("Cartella dove salvare i quattro stem",sourceFile.getParentDirectory(),"*");juce::Component::SafePointer<Studio> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories,[safe](const juce::FileChooser& dialog){if(auto* self=safe.getComponent()){
            const auto parent=dialog.getResult();if(!parent.isDirectory())return;const auto destination=parent.getNonexistentChildFile(self->sourceFile.getFileNameWithoutExtension()+"-stems","",false);const auto audio=self->current;
            self->launch("Separazione Open-Unmix: voce, batteria, basso e altro…",[self,audio,destination]{TemporaryFolder temporary;const auto input=temporary.folder.getChildFile("input.wav");writeAudio(input,*audio);auto command=self->stemCommand("separate");command.addArray({input.getFullPathName(),destination.getFullPathName(),"--model-dir",stemsInstallation().getChildFile("models").getFullPathName(),"--threads","2","--cancel-file",temporary.folder.getChildFile("cancel").getFullPathName()});(void)childCommand(command,self->cancelled,temporary.folder.getChildFile("cancel"));if(!destination.getChildFile("stems.json").existsAsFile())throw std::runtime_error("Separazione incompleta: manifest assente");return destination;},[audio](Studio& s,juce::File folder){s.stemFolder=folder;s.stemSource=audio;for(auto& slider:s.stemGains)slider.setValue(0);s.setStatus("Quattro stem salvati. Regola i livelli e premi Applica rebalance.");});
        }});
    }
    void renderStemMix(){
        if(!current||current!=stemSource.lock())return;
        const auto source=current;const auto folder=stemFolder;std::array<double,4> gains{};for(std::size_t i=0;i<gains.size();++i)gains[i]=stemGains[i].getValue();
        launch("Render del nuovo bilanciamento…",[this,source,folder,gains]{TemporaryFolder temporary;const auto output=temporary.folder.getChildFile("rebalance.wav");auto command=stemCommand("rebalance");command.addArray({folder.getFullPathName(),output.getFullPathName(),"--vocals-db="+juce::String(gains[0],1),"--drums-db="+juce::String(gains[1],1),"--bass-db="+juce::String(gains[2],1),"--other-db="+juce::String(gains[3],1),"--cancel-file",temporary.folder.getChildFile("cancel").getFullPathName()});(void)childCommand(command,cancelled,temporary.folder.getChildFile("cancel"));auto result=readAudio(output);if(result->size()!=source->size()||result->rate!=source->rate||result->channels.size()!=source->channels.size())throw std::runtime_error("Rebalance con formato o durata inattesi");return picture(result,"Rebalance applicato. I quattro stem separati restano nella cartella scelta.");},[](Studio& s,RenderResult result){s.commit(std::move(result));s.stemSource=s.current;});
    }
    void configureModule(){
        const int id=module.getSelectedId();parameter.setVisible(true);parameterLabel.setVisible(true);parameter.setTextValueSuffix("");
        constexpr std::array<double,27> defaults{35,60,20,20,100,80,50,80,50,60,100,40,50,100,50,60,100,100,100,100,100,100,100,100,100,100,100};
        amount.setValue(defaults[static_cast<std::size_t>(juce::jlimit(1,27,id)-1)],juce::dontSendNotification);
        amount.setEnabled(id<17);amountLabel.setText(id<17?"Intensità / miscela":"Operazione completa",juce::dontSendNotification);
        apply.setButtonText(id==25?"Elabora file intero":"Elabora selezione");
        juce::String explanation="Il trattamento viene calcolato sul file e applicato alla selezione. Usa Prima / Rimosso per controllare il risultato.";
        parameter.setRange(0,100,1);parameter.setValue(0);parameterLabel.setText("Parametro",juce::dontSendNotification);
        if(id==5){parameterLabel.setText("Soglia clipping · 0 = auto",juce::dontSendNotification);parameter.setTextValueSuffix(" % FS");explanation="SPADE ricostruisce i picchi tagliati. Può creare picchi oltre 0 dBFS: esporta float32 o normalizza dopo. Il render può durare più dell'audio.";}
        else if(id==7){parameter.setRange(1,10,1);parameter.setValue(5);parameterLabel.setText("Sensibilità",juce::dontSendNotification);}
        else if(id==9){parameter.setRange(2000,12000,100);parameter.setValue(4000);parameter.setTextValueSuffix(" Hz");parameterLabel.setText("Inizio banda sibilanti",juce::dontSendNotification);}
        else if(id==11){parameter.setRange(30,300,1);parameter.setValue(80);parameter.setTextValueSuffix(" Hz");parameterLabel.setText("Frequenza low cut",juce::dontSendNotification);}
        else if(id==15){parameter.setRange(-60,0,.1);parameter.setValue(-32);parameter.setTextValueSuffix(" dBFS");parameterLabel.setText("Soglia Source",juce::dontSendNotification);}
        else if(id==17){parameter.setRange(-30,-9,.1);parameter.setValue(-16);parameter.setTextValueSuffix(" LUFS");parameterLabel.setText("Loudness obiettivo",juce::dontSendNotification);explanation="Normalizzazione della selezione con obiettivo true peak −1 dBTP. Controlla poi le statistiche del file completo.";}
        else if(id==18){parameter.setRange(-24,0,.1);parameter.setValue(-1);parameter.setTextValueSuffix(" dBFS");parameterLabel.setText("Picco obiettivo",juce::dontSendNotification);}
        else if(id==19){parameter.setRange(-24,24,.1);parameter.setValue(0);parameter.setTextValueSuffix(" dB");parameterLabel.setText("Guadagno",juce::dontSendNotification);}
        else{parameter.setVisible(false);parameterLabel.setVisible(false);}
        if(id==1)explanation="Seleziona almeno 2,1 secondi di solo rumore e premi Apprendi. Poi seleziona la parte da pulire e applica il trattamento.";
        if(id==2)explanation="Rete RNNoise compatta inclusa. Pensata per parlato, soprattutto mono; può alterare musica e immagine stereo. Il cambio di frequenza è gestito offline.";
        if(id==4){explanation="WPE è adatto soprattutto al parlato. Può attenuare fortemente note sostenute e code volute: parti con miscela bassa e confronta Prima / Rimosso.";}
        if(id==13)explanation="Trascina un rettangolo nello spettrogramma: vengono attenuate solo le frequenze e il tempo selezionati, con bordi sfumati.";
        if(id==14)explanation="Ricostruzione di un breve intervallo usando il contesto. Ingrandisci e seleziona pochi campioni: il massimo è 4096, non una garanzia di recupero di parole mancanti.";
        if(id==16)explanation="Scegli un file di riferimento: EQ Match confronta lo spettro medio. Non è una separazione di voce, basso e batteria.";
        if(id==25)explanation="Elabora il file intero seguendo un tono stabile. Può cambiare durata e immagine stereo. Se cambia durata, Prima / Rimosso e Ripristina non sono disponibili; Annulla torna alla versione precedente.";
        if(id>=26)explanation="Elaborazione Mid/Side del segnale stereo, senza separazione neurale. Coinvolge tutti gli elementi nella stessa posizione panoramica.";
        help.setText(explanation,juce::dontSendNotification);
    }
    void render(){
        if(!current||selection.end<=selection.start)return;
        const auto audio=current;const auto range=selection;const auto learned=noiseProfile;const int id=module.getSelectedId();const float mix=static_cast<float>(amount.getValue()*.01);const double value=parameter.getValue();const auto referencePath=referenceFile;
        launch("Elaborazione: "+module.getText()+"…",[this,audio,range,learned,id,mix,value,referencePath]{
            auto result=std::make_shared<OfflineAudio>(*audio);juce::String note="Trattamento completato. Ascolta Prima / Rimosso e usa Annulla se necessario.";
            if(mix<=0)return picture(result,"Intensità zero: audio invariato");
            if(id==1||id==3||id==15){sculptclean::Parameters p;p.denoise=id==1?mix:0;p.dereverb=id==3?mix:0;p.sourceEnabled=id==15;p.sourceThresholdDb=static_cast<float>(value);p.sourceDepth=mix;auto wet=sculptclean::offlineCore(*audio,p,learned,&cancelled,&progress);sculptclean::offlineBlendSelection(*result,wet,range);}
            else if(id==2){auto wet=sculptclean::offlineVoice(*audio,mix,&cancelled,&progress);sculptclean::offlineBlendSelection(*result,wet,range);}
            else if(id==5||id==14){float* pointers[]{result->channels[0].data(),result->channels.size()>1?result->channels[1].data():nullptr};sculptclean::OfflineRepair::Report report;
                if(id==5){sculptclean::OfflineRepair::DeclipOptions options;options.threshold=static_cast<float>(value*.01);options.amount=mix;report=sculptclean::OfflineRepair::declip(pointers,static_cast<int>(result->channels.size()),result->size(),result->rate,options,&cancelled,&progress);for(std::size_t c=0;c<result->channels.size();++c){std::copy(audio->channels[c].begin(),audio->channels[c].begin()+static_cast<std::ptrdiff_t>(range.start),result->channels[c].begin());std::copy(audio->channels[c].begin()+static_cast<std::ptrdiff_t>(range.end),audio->channels[c].end(),result->channels[c].begin()+static_cast<std::ptrdiff_t>(range.end));}}
                else{sculptclean::OfflineRepair::RepairOptions options;options.amount=mix;report=sculptclean::OfflineRepair::repairRegion(pointers,static_cast<int>(result->channels.size()),result->size(),result->rate,range.start,range.end-range.start,options,&cancelled,&progress);}
                if(!report.ok||report.cancelled)throw std::runtime_error(report.message);
                note=juce::String::fromUTF8(report.message.c_str())+" · Picco "+juce::String(juce::Decibels::gainToDecibels(report.peakAfter,-120.0f),1)+" dBFS";}
            else if(id==13)*result=sculptclean::offlineSpectralAttenuate(*audio,range,-48*mix,&cancelled);
            else if((id>=18&&id<=24)||id>=26){
                double gain=1;if(id==18){double peak=0;for(const auto& channel:audio->channels)for(auto n=range.start;n<range.end;++n)peak=std::max(peak,std::abs(static_cast<double>(channel[n])));gain=peak>1e-12?std::pow(10.0,value/20)/peak:1;}if(id==19)gain=std::pow(10.0,value/20);
                for(std::size_t c=0;c<result->channels.size();++c){double mean=0;if(id==20){for(auto n=range.start;n<range.end;++n)mean+=audio->channels[c][n];mean/=static_cast<double>(range.end-range.start);}for(auto n=range.start;n<range.end;++n){if(n%8192==0)sculptclean::offlineCheckCancel(&cancelled);const double t=static_cast<double>(n-range.start)/std::max<std::size_t>(1,range.end-range.start-1);double wet=audio->channels[c][n];if(id==18||id==19)wet*=gain;else if(id==20)wet-=mean;else if(id==21)wet*=t;else if(id==22)wet*=1-t;else if(id==23)wet=-wet;else if(id==24&&audio->channels.size()==2)wet=audio->channels[1-c][n];else if(id==26&&audio->channels.size()==2)wet=.5*(audio->channels[0][n]+audio->channels[1][n]);else if(id==27&&audio->channels.size()==2)wet=(c==0?.5:-.5)*(audio->channels[0][n]-audio->channels[1][n]);result->channels[c][n]+=static_cast<float>(mix*(wet-audio->channels[c][n]));}}
            }
            else{
                const bool level=id==17;const auto margin=level?0:static_cast<std::size_t>(audio->rate*.25);const auto begin=id==25?0:(range.start>margin?range.start-margin:0);const auto end=id==25?audio->size():std::min(audio->size(),range.end+margin);OfflineAudio crop;crop.rate=audio->rate;for(const auto& channel:audio->channels)crop.channels.emplace_back(channel.begin()+static_cast<std::ptrdiff_t>(begin),channel.begin()+static_cast<std::ptrdiff_t>(end));
                TemporaryFolder temporary;const auto input=temporary.folder.getChildFile("input.wav"),output=temporary.folder.getChildFile("output.wav");writeAudio(input,crop);juce::StringArray command;const juce::File nice("/usr/bin/nice");if(nice.existsAsFile())command.addArray({nice.getFullPathName(),"-n","10"});command.add(catharBinary().getFullPathName());juce::StringArray options;juce::String name;
                if(id==4){name="dereverb";options={"--wpe","--taps","10","--delay","3"};}
                else if(id==6){name="declick";options={"--threshold","10","--method","ar"};}
                else if(id==7){name="decrackle";options={"--sensitivity",juce::String(value,0)};}
                else if(id==8){name="dehum";options={"--freq","0","--harmonics","8","--adaptive"};}
                else if(id==9){name="deesser";options={"--freq",juce::String(value,0),"--threshold","6","--bands","3"};}
                else if(id==10){name="deplosive";options={"--strength","4","--method","events"};}
                else if(id==11){name="dewind";options={"--cutoff",juce::String(value,0)};}
                else if(id==12){name="derustle";options={"--strength","4"};}
                else if(id==16){if(!referencePath.existsAsFile())throw std::runtime_error("Scegli prima un file di riferimento per EQ Match");name="rebalance";options={"--reference",referencePath.getFullPathName(),"--strength","1"};}
                else if(id==17){name="normalize";options={"--target="+juce::String(value,1),"--true-peak=-1"};}
                else if(id==25)name="dewow";else throw std::runtime_error("Modulo non disponibile");
                command.add(name);command.add(input.getFullPathName());command.addArray(options);command.addArray({"--out",output.getFullPathName()});(void)childCommand(command,cancelled);auto processed=readAudio(output);
                if(processed->rate!=crop.rate||processed->channels.size()!=crop.channels.size()||(id!=25&&processed->size()!=crop.size()))throw std::runtime_error("Il motore ha cambiato formato o durata: risultato non applicato");
                if(id==25){result=processed;note="Wow / Flutter applicato al file intero. Durata: "+juce::String(result->size()/result->rate,3)+" s. Usa Annulla per tornare alla versione precedente.";}
                else if(level){for(std::size_t c=0;c<result->channels.size();++c)std::copy(processed->channels[c].begin(),processed->channels[c].end(),result->channels[c].begin()+static_cast<std::ptrdiff_t>(begin));}
                else{OfflineAudio wet=*audio;for(std::size_t c=0;c<wet.channels.size();++c)std::copy(processed->channels[c].begin(),processed->channels[c].end(),wet.channels[c].begin()+static_cast<std::ptrdiff_t>(begin));sculptclean::offlineBlendSelection(*result,wet,range,mix);}
            }
            for(const auto& channel:result->channels)for(float sample:channel)if(!std::isfinite(sample))throw std::runtime_error("Risultato non finito: operazione scartata");
            return picture(result,note);
        },[](Studio& s,RenderResult result){s.commit(std::move(result));});
    }
    juce::LookAndFeel_V4 look;
    juce::TextButton open{"Apri"},save{"Esporta"},undo{"Annulla"},redo{"Ripeti"},play{"Ascolta"},stop{"Stop"},original{"Prima"},removed{"Rimosso"},apply{"Elabora selezione"},learn{"Apprendi rumore"},cancel{"Interrompi"},all{"Seleziona tutto"},zoom{"Zoom selezione"},unzoom{"Vista intera"},restore{"Ripristina selezione"},reference{"Riferimento EQ…"},statistics{"Statistiche / LUFS"},audioSettings{"Uscita audio…"},setupStems{"Prepara separazione"},separateStems{"Separa 4 stem"},rebalanceStems{"Applica rebalance"},revealStems{"Cartella stem"};
    juce::ComboBox module,exportRate;juce::Slider amount,parameter;juce::Label status,help,amountLabel,parameterLabel,selectionLabel;AudioCanvas canvas;
    double progressDisplay=0;juce::ProgressBar progressBar;std::atomic<float> progress{0};std::atomic<bool> cancelled{false};bool busy=false;
    std::thread worker;std::shared_ptr<OfflineAudio> current,initial,savedSnapshot;std::vector<std::shared_ptr<OfflineAudio>> undoStack,redoStack;OfflineSelection selection;sculptclean::LearnedState noiseProfile;
    std::unique_ptr<juce::FileChooser> chooser;juce::File sourceFile,referenceFile;
    std::array<juce::Slider,4> stemGains;std::array<juce::Label,4> stemLabels;juce::File stemFolder;std::weak_ptr<OfflineAudio> stemSource;
    juce::AudioTransportSource transport;std::unique_ptr<PreviewSource> memorySource;double previewStart=0;
};
class MainWindow final:public juce::DocumentWindow {
public:
    MainWindow():DocumentWindow("Sculpt Clean Studio",background,DocumentWindow::allButtons){setUsingNativeTitleBar(true);setContentOwned(new Studio,true);setResizable(true,true);setResizeLimits(1120,760,2200,1400);centreWithSize(1240,820);setVisible(true);}
    void closeButtonPressed()override{juce::JUCEApplication::getInstance()->systemRequestedQuit();}
    void requestClose(std::function<void()> close){if(auto* studio=dynamic_cast<Studio*>(getContentComponent()))studio->requestClose(std::move(close));}
};

void studioSelfTest(const juce::File& screenshot){
    auto require=[](bool condition,const char* message){if(!condition)throw std::runtime_error(message);};
    auto audio=std::make_shared<OfflineAudio>();audio->rate=48000;audio->channels.assign(2,std::vector<float>(48000));
    for(std::size_t n=0;n<audio->size();++n){const auto sample=static_cast<float>(.2*std::sin(6.283185307179586*997*static_cast<double>(n)/audio->rate));audio->channels[0][n]=sample;audio->channels[1][n]=-sample;}
    audio->channels[0][0]=1.25f;audio->channels[1][0]=-.75f;
    TemporaryFolder temporary;const auto file=temporary.folder.getChildFile("float.wav");writeAudio(file,*audio);auto decoded=readAudio(file);
    require(decoded->rate==audio->rate&&decoded->channels==audio->channels,"WAV float32 round-trip failed");
    OfflineSelection range{17,1041,0,24000};PreviewSource preview(audio,audio,range,false,false);
    juce::AudioBuffer<float> buffer(2,256);std::size_t position=0;
    for(int block=0;block<6;++block){preview.getNextAudioBlock({&buffer,0,256});for(int c=0;c<2;++c)for(int n=0;n<256;++n){const auto index=position+static_cast<std::size_t>(n);const float expected=index<range.end-range.start?audio->channels[static_cast<std::size_t>(c)][range.start+index]:0;require(buffer.getSample(c,n)==expected,"Preview bounds or sample mismatch");}position+=256;}
    require(preview.getNextReadPosition()>preview.getTotalLength()+1,"Preview does not advance beyond EOF");
    preview.setNextReadPosition(0);juce::AudioTransportSource transport;transport.setSource(&preview,0,nullptr,48000);transport.prepareToPlay(256,48000);transport.start();
    for(int block=0;block<12;++block)transport.getNextAudioBlock({&buffer,0,256});
    require(!transport.isPlaying(),"Transport remains playing after EOF");transport.setSource(nullptr);transport.releaseResources();
    std::atomic<bool> cancel{false};const auto statistics=childCommand({catharBinary().getFullPathName(),"stats",file.getFullPathName()},cancel);
    require(statistics.isNotEmpty(),"Bundled offline engine produced no statistics");
    for(const auto* name:{"sculpt-stems.py","setup-stems.sh","requirements-stems.txt","stems-models.json"})require(stemsResources().getChildFile(name).existsAsFile(),"Stems resource missing from app bundle");
    Studio studio(false);studio.loadSelfTestAudio(audio);auto image=studio.createComponentSnapshot(studio.getLocalBounds());
    require(image.isValid()&&image.getWidth()==1240&&image.getHeight()==820,"Studio image rendering failed");
    require(screenshot.getParentDirectory().createDirectory().wasOk(),"Unable to create screenshot directory");
    auto stream=screenshot.createOutputStream();require(stream!=nullptr,"Unable to open screenshot output");
    juce::PNGImageFormat format;require(format.writeImageToStream(image,*stream),"Unable to save Studio screenshot");
    std::cout<<"Studio self-test passed: float32 WAV, preview/EOF, bundled Cathar, resources, interface snapshot\n";
}

class Application final:public juce::JUCEApplication {
public:
    const juce::String getApplicationName()override{return "Sculpt Clean Studio";}
    const juce::String getApplicationVersion()override{return "0.2.0";}
    bool moreThanOneInstanceAllowed()override{return false;}
    void initialise(const juce::String&)override{
        const auto arguments=getCommandLineParameterArray();
        if(arguments.contains("--self-test")){
            try{const int index=arguments.indexOf("--self-test");if(index+1>=arguments.size())throw std::runtime_error("--self-test requires a screenshot path");studioSelfTest(juce::File::getCurrentWorkingDirectory().getChildFile(arguments[index+1]));setApplicationReturnValue(0);}
            catch(const std::exception& error){std::cerr<<"Studio self-test failed: "<<error.what()<<'\n';setApplicationReturnValue(1);}
            quit();return;
        }
        window=std::make_unique<MainWindow>();
    }
    void systemRequestedQuit()override{if(window)window->requestClose([]{juce::JUCEApplication::quit();});else quit();}
    void shutdown()override{window.reset();}
private:std::unique_ptr<MainWindow> window;
};
}
START_JUCE_APPLICATION(Application)
