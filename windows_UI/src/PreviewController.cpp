#include "PreviewController.hpp"
#include <QColorSpace>
#include <QMetaObject>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
namespace desktop=spk::desktop;
namespace output=desktop::output;
namespace editing=desktop::editing;
QString text(const std::string& s) { return QString::fromUtf8(s.data(),qsizetype(s.size())); }
std::filesystem::path path(const QUrl& u) { return std::filesystem::path(u.toLocalFile().toStdWString()); }
QVariantList list(const std::vector<desktop::Stock>& stocks) {
    QVariantList result;
    for(const auto& s:stocks) result.push_back(QVariantMap{{"id",text(s.id)},{"name",text(s.label)},{"positive",s.is_positive}});
    return result;
}
bool sameSettings(desktop::RenderSettings a, desktop::RenderSettings b) {
    const bool exposure=std::abs(a.print_exposure-b.print_exposure)<1e-12;
    a.print_exposure=b.print_exposure;
    return exposure&&a==b;
}
bool sameState(const editing::State& a,const editing::State& b) {
    return sameSettings(a.settings,b.settings)&&a.mode==b.mode&&a.geometry==b.geometry;
}
QImage visibleImage(const desktop::FramePtr& frame,const QImage& full,const output::Geometry& geometry) {
    if(geometry.quarterTurns==0&&!geometry.flipHorizontal&&!geometry.flipVertical&&geometry.straightenDegrees==0)
        return output::displayImage(full,geometry.crop);
    return output::displayFrame(frame,geometry);
}
output::ExportOptions exportOptions(int format,int quality,int maxEdge) {
    if(format<0||format>3||quality<1||quality>100||maxEdge<0||maxEdge>100000)
        throw std::runtime_error("Invalid export options");
    return {static_cast<output::Format>(format),quality,maxEdge};
}
// Median Rec.601 luma of a small downscale. The median tracks the typical
// pixel, so skies and clipped highlights do not skew the match. The paper
// curve's nonlinearity is handled by the caller's iterative refinement.
double medianLuma(const QImage& image) {
    if(image.isNull())throw std::runtime_error("Cannot measure an empty image");
    const auto small=image.scaled(512,512,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    std::vector<double> luma;
    luma.reserve(std::size_t(small.width())*std::size_t(small.height()));
    for(int y=0;y<small.height();++y) {
        const auto* line=reinterpret_cast<const QRgb*>(small.constScanLine(y));
        for(int x=0;x<small.width();++x)
            luma.push_back(0.299*qRed(line[x])+0.587*qGreen(line[x])+0.114*qBlue(line[x]));
    }
    if(luma.empty())throw std::runtime_error("Cannot measure an empty image");
    const auto mid=luma.begin()+std::ptrdiff_t(luma.size()/2);
    std::nth_element(luma.begin(),mid,luma.end());
    return *mid;
}
}
QImage PreviewController::imageFromFrame(const desktop::FramePtr& frame) {
    // QImage is implicitly shared; the cleanup holder keeps the independently
    // owned engine result and its display bytes alive on the scene-graph side.
    auto held=std::make_unique<desktop::FramePtr>(frame);
    QImage image(frame->bgra8.data(),int(frame->width()),int(frame->height()),
                 qsizetype(frame->width())*4,QImage::Format_ARGB32,
                 [](void* p){delete static_cast<desktop::FramePtr*>(p);},held.get());
    if(image.isNull()) throw std::runtime_error("Cannot create the display image");
    held.release();
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

PreviewController::PreviewController(std::filesystem::path resources,QObject* parent,ImageFactory factory,QString editDirectory)
    :QObject(parent),resources_(resources),
     edit_store_(editDirectory.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/Edits":editDirectory),
     image_factory_(factory?std::move(factory):ImageFactory(imageFromFrame)) {
    auto_apply_.setSingleShot(true);auto_apply_.setInterval(180);
    connect(&auto_apply_,&QTimer::timeout,this,[this]{if(dirty())apply();});
    // A throttle, not a debounce: keeps firing while the user keeps dragging,
    // so live-tier previews (~10-30 ms) stream during the gesture and the
    // full-resolution render still follows once editing settles.
    live_timer_.setSingleShot(true);live_timer_.setInterval(50);
    connect(&live_timer_,&QTimer::timeout,this,[this]{
        if(frame_&&dirty()&&!dialog_open_&&!busy_&&gesture_key_!="straightenDegrees")applyLive();
        if(frame_&&dirty()&&!dialog_open_)live_timer_.start();
    });
    save_timer_.setSingleShot(true);save_timer_.setInterval(350);
    connect(&save_timer_,&QTimer::timeout,this,&PreviewController::flushEdits);
    worker_=std::thread([this,resources=std::move(resources)]{workerLoop(resources);});
}
PreviewController::~PreviewController() {
    auto_apply_.stop();
    live_timer_.stop();
    endEditGesture();flushEdits();
    if(cancel_batch_)cancel_batch_->store(true);
    if(cancel_gallery_)cancel_gallery_->store(true);
    if(cancel_balance_)cancel_balance_->store(true);
    ++generation_;
    {std::lock_guard lock(mutex_);stopping_=true;jobs_.clear();}
    wake_.notify_one();
    if(worker_.joinable()) worker_.join();
    {std::lock_guard lock(mutex_);low_jobs_.clear();}
}
void PreviewController::workerLoop(std::filesystem::path resources) {
    std::unique_ptr<desktop::PreviewHost> host;
    Reply init;init.operation="init";
    try {host=std::make_unique<desktop::PreviewHost>(resources);init.catalog=host->catalog();}
    catch(const std::exception& e){init.error=text(e.what());}
    deliver(std::move(init));
    if(!host)return;
    for(;;) {
        Job job;
        {std::unique_lock lock(mutex_);
         wake_.wait(lock,[&]{return stopping_||!jobs_.empty()||!low_jobs_.empty();});
         if(stopping_)break;
         // User operations always run first; thumbnails fill the idle time.
         if(!jobs_.empty()) job=std::move(jobs_.front()),jobs_.pop_front();
         else job=std::move(low_jobs_.front()),low_jobs_.pop_front();}
        deliver(job(*host));
    }
}
void PreviewController::deliver(Reply reply) {
    QMetaObject::invokeMethod(this,[this,reply=std::move(reply)]() mutable {accept(std::move(reply));},Qt::QueuedConnection);
}
void PreviewController::submit(Job job,QString status,QString operation) {
    auto_apply_.stop();active_operation_=std::move(operation);
    busy_=true;failed_=false;status_=std::move(status);emit stateChanged();emit historyChanged();
    {std::lock_guard lock(mutex_);jobs_.push_back(std::move(job));}
    wake_.notify_one();
}
void PreviewController::submitQuiet(Job job) {
    // Live previews ride the same serial worker without touching busy_,
    // status or the debounce: a queued full render simply runs after them.
    {std::lock_guard lock(mutex_);jobs_.push_back(std::move(job));}
    wake_.notify_one();
}
void PreviewController::applyLive() {
    if(!frame_||busy_||dialog_open_)return;
    const auto s=settings();const auto shown=frame_;
    const auto gen=generation_;const auto revision=settings_revision_;
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    submitQuiet([s,shown,gen,revision,mode](auto& host){
        Reply r;r.generation=gen;r.settings_revision=revision;r.operation="live-render";
        try {
            // Live feedback is best effort: if the worker holds another
            // photo, skip and let the full render path reconcile.
            const auto active=host.current();
            if(active&&active->source==shown->source&&active->decode_mode==mode)
                r.frame=host.render_variant(s);
        }catch(const std::exception&){}
        return r;
    });
}
void PreviewController::accept(Reply reply) {
    if(reply.generation!=generation_)return; // a retired generation never replaces the canvas
    if(reply.operation=="live-render") {
        // Interactive live-tier feedback only: touches the displayed image,
        // never the held full-resolution frame, history or busy state. A
        // reply at or below the last full publication is stale low resolution.
        if(reply.frame&&reply.settings_revision>last_full_revision_) {
            try {
                auto live=image_factory_(reply.frame);
                if(!live.isNull()) {
                    image_=visibleImage(reply.frame,live,geometry_);
                    emit frameChanged();
                }
            }catch(const std::exception&){}
        }
        return;
    }
    const bool superseded=reply.operation=="render"&&reply.settings_revision!=settings_revision_;
    busy_=false;active_operation_.clear();failed_=!superseded&&!reply.error.isEmpty();
    if(superseded) {
        // The worker may finish an earlier choice after the next click. Keep
        // both the displayed frame and the newest desired controls; never flash
        // the retired result or restore its settings over the user's last edit.
        status_=dirty()?QStringLiteral("正在更新预览…"):QStringLiteral("预览已是当前选择");
    } else if(failed_) {
        status_=reply.error;
        // The gallery never changes the shown settings; a failure there must
        // not roll the controls back to the last rendered frame either.
        if(frame_&&reply.operation!="export"&&reply.operation!="batch-export"
            &&reply.operation!="effects-gallery"&&reply.operation!="balance-brightness")restoreSettings();
    } else if(reply.operation=="init") {
        ready_=true;catalog_=std::move(reply.catalog);films_=list(catalog_.films);papers_=list(catalog_.papers);
        restoreSettings();emit catalogChanged();status_=QStringLiteral("就绪 · 打开 RAW 开始");
    } else if(reply.frame) {
        // Allocate the presentation wrapper before publishing any new UI state.
        try {
            auto next=image_factory_(reply.frame);
            if(next.isNull())throw std::runtime_error("Cannot create the display image");
            const auto geometry=reply.operation=="open"?reply.geometry:geometry_;
            auto visible=visibleImage(reply.frame,next,geometry);
            frame_=std::move(reply.frame);full_image_=std::move(next);image_=std::move(visible);
            geometry_=geometry;
            if(reply.operation=="open")active_index_=reply.library_index;
        }
        catch(const std::exception& e){failed_=true;status_=text(e.what());if(frame_)restoreSettings();}
        if(!failed_) {
            restoreSettings();
            updateThumbnail();
            // Any still-in-flight live preview is now coarser than what is on
            // screen; record the publication so late replies get dropped.
            last_full_revision_=settings_revision_;
            // A different photo makes the retained gallery cells stale; drop
            // them (~140 MB of live-tier images) instead of carrying them.
            if(reply.operation=="open"&&!gallery_films_.isEmpty()) {
                gallery_films_.clear();gallery_progress_=gallery_total_=0;
                emit galleryChanged();
            }
            status_=QStringLiteral("渲染完成 %1 秒%2")
                .arg(frame_->timings.render_ms/1000,0,'f',2)
                .arg(frame_->reprint()?QStringLiteral(" · 已复用胶片"):QString());
            emit frameChanged();emit geometryChanged();if(reply.operation=="open")emit sourceOpened();
        }
    } else if(reply.operation=="export") status_=QStringLiteral("已导出当前画面");
    else if(reply.operation=="batch-export")status_=reply.message;
    else if(reply.operation=="effects-gallery"||reply.operation=="balance-brightness")status_=reply.message;
    if(failed_&&reply.operation=="open"&&reply.library_index>=0&&reply.library_index<int(library_.size()))
        library_[reply.library_index].error=status_;
    if(failed_&&reply.operation!="export"&&reply.operation!="batch-export"
        &&reply.operation!="effects-gallery"&&reply.operation!="balance-brightness"&&active_index_>=0){
        auto& history=library_[active_index_].undo;
        while(!history.empty()&&sameState(history.back(),currentState()))history.pop_back();
    }
    if(reply.operation=="batch-export")cancel_batch_.reset();
    if(reply.operation=="effects-gallery")cancel_gallery_.reset();
    if(reply.operation=="balance-brightness")cancel_balance_.reset();
    emit libraryChanged();
    emit historyChanged();
    emit stateChanged();
    if(reply.operation=="init")emit initialized(!failed_);
    else emit operationFinished(reply.operation,!failed_);
    // Export holds an immutable displayed frame. An edit made just before it
    // began still needs applying, including after an overwrite refusal. The
    // balance pass renders the reference on the worker without publishing it,
    // so a pending edit there needs the same follow-up.
    if((superseded||reply.operation=="export"||reply.operation=="batch-export"
        ||reply.operation=="balance-brightness")&&!busy_)schedulePreview();
    // Any successful operation may have cleared thumbnails (sync, batch
    // applies) or imported photos the user never visited: fill them in.
    if(!failed_)scheduleThumbnails();
}
desktop::RenderSettings PreviewController::settings() const {
    desktop::RenderSettings s=extra_settings_;
    if(film_index_>=0&&std::size_t(film_index_)<catalog_.films.size())s.film_stock=catalog_.films[film_index_].id;
    if(paper_index_>=0&&std::size_t(paper_index_)<catalog_.papers.size())s.print_stock=catalog_.papers[paper_index_].id;
    s.print_exposure=std::exp2(-exposure_ev_);return s;
}
void PreviewController::restoreSettings() {
    const auto s=frame_?frame_->settings:desktop::RenderSettings{};
    loadSettings(s,frame_?frame_->decode_mode:desktop::DecodeMode::compatible16);
    saveActiveSettings();
}
void PreviewController::loadSettings(const desktop::RenderSettings& s,desktop::DecodeMode mode) {
    extra_settings_=s;
    for(std::size_t i=0;i<catalog_.films.size();++i)if(catalog_.films[i].id==s.film_stock)film_index_=int(i);
    for(std::size_t i=0;i<catalog_.papers.size();++i)if(catalog_.papers[i].id==s.print_stock)paper_index_=int(i);
    exposure_ev_=-std::log2(s.print_exposure);
    decode_mode_=mode==desktop::DecodeMode::headroom?1:0;
    emit settingsChanged();
}
bool PreviewController::dirty() const {
    if(!frame_)return false;
    return !sameSettings(settings(),frame_->settings)||
        decode_mode_!=(frame_->decode_mode==desktop::DecodeMode::headroom?1:0);
}
bool PreviewController::filmIsPositive() const {
    return film_index_>=0&&std::size_t(film_index_)<catalog_.films.size()&&catalog_.films[film_index_].is_positive;
}
QString PreviewController::fileName() const {return frame_?QString::fromStdWString(frame_->source.filename().wstring()):QString();}
QUrl PreviewController::sourceFolder() const {return frame_?QUrl::fromLocalFile(QString::fromStdWString(frame_->source.parent_path().wstring())):QUrl();}
QVariantMap PreviewController::metadata() const {
    if(!frame_)return {};
    return {{"width",image_.width()},{"height",image_.height()},
        {"sourceWidth",frame_->width()},{"sourceHeight",frame_->height()},
        {"camera",text(frame_->metadata.make+" "+frame_->metadata.model)},
        {"decodeMs",frame_->timings.decode_ms},{"renderMs",frame_->timings.render_ms},
        {"totalMs",frame_->timings.total_ms},{"cached",frame_->reprint()},{"scanFilm",frame_->scan_film},
        {"mode",frame_->decode_mode==desktop::DecodeMode::headroom?"headroom":"compatible16"}};
}
void PreviewController::schedulePreview(){if(!dialog_open_&&gesture_key_!="straightenDegrees"&&frame_&&dirty()&&!auto_apply_.isActive())auto_apply_.start();}
void PreviewController::setDialogOpen(bool open){
    if(dialog_open_==open)return;
    dialog_open_=open;
    if(open){endEditGesture();auto_apply_.stop();}else schedulePreview();
    emit stateChanged();emit historyChanged();
}
void PreviewController::changed(const QString& editKey){
    if(gesture_index_>=0&&gesture_key_!=editKey)endEditGesture();
    saveActiveSettings(true);
    ++settings_revision_;auto_apply_.stop();schedulePreview();
    if(!live_timer_.isActive())live_timer_.start();
    if(frame_)status_=QStringLiteral("正在更新预览…");
    failed_=false;emit settingsChanged();emit stateChanged();
}
void PreviewController::setFilmIndex(int v){if(!settingsEditable()||v<0||v>=films_.size()||v==film_index_)return;film_index_=v;changed("filmIndex");}
void PreviewController::setPaperIndex(int v){if(!settingsEditable()||filmIsPositive()||v<0||v>=papers_.size()||v==paper_index_)return;paper_index_=v;changed("paperIndex");}
void PreviewController::setDecodeMode(int v){if(!settingsEditable()||v<0||v>1||v==decode_mode_)return;decode_mode_=v;changed("decodeMode");}
void PreviewController::setExposureEv(double v){if(!settingsEditable()||filmIsPositive()||!std::isfinite(v)||v< -2||v>2||v==exposure_ev_)return;exposure_ev_=v;changed("exposureEv");}
void PreviewController::resetSettings(){if(!settingsEditable())return;loadSettings({},desktop::DecodeMode::compatible16);changed();}
void PreviewController::openRaw(const QUrl& url) {
    // Changing sources and exporting remain single flight. Parameter edits
    // can be coalesced during a render, but cannot retarget an in-flight open.
    if(!ready_||busy_||!url.isLocalFile())return;
    saveActiveSettings();
    const int index=addFile(url);if(index<0)return;
    // A known photo restores its own saved settings. A genuinely new photo
    // still starts with the current preset; opening never overwrites a sidecar.
    openItem(index);
}
void PreviewController::openItem(int index) {
    endEditGesture();
    const auto item=library_.at(index);const auto p=path(item.url);
    const auto gen=++generation_;
    submit([p,item,gen,index](auto& host){Reply r;r.generation=gen;r.operation="open";r.library_index=index;r.geometry=item.geometry;
        try{r.frame=host.open_raw(p,item.mode,item.settings);}catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在解码并渲染 RAW…"),"open");
}
void PreviewController::apply() {
    if(!frame_||busy_||dialog_open_)return;
    const auto s=settings();const auto shown=frame_;const auto gen=++generation_;const auto revision=settings_revision_;
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    submit([s,shown,gen,revision,mode](auto& host){Reply r;r.generation=gen;r.settings_revision=revision;r.operation="render";
        try {
            // Publication can fail after a successful worker open (for example,
            // a display allocation failure). Reconcile against the image the
            // user is actually editing before reusing the worker's session.
            const auto active=host.current();
            r.frame=active&&active->source==shown->source&&active->decode_mode==mode
                ?host.render(s):host.open_raw(shown->source,mode,s);
        }catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在更新胶片效果…"),"render");
}
void PreviewController::exportTiff(const QUrl& url) {
    exportImage(url,0,95,0);
}
void PreviewController::exportImage(const QUrl& url,int format,int quality,int maxLongEdge) {
    if(!frame_||busy_||!url.isLocalFile())return;
    const auto p=path(url);const auto held=frame_;const auto gen=++generation_;const auto geometry=geometry_;const auto resources=resources_;
    submit([p,held,gen,geometry,resources,format,quality,maxLongEdge](auto&){Reply r;r.generation=gen;r.operation="export";
        try{output::exportFrame(held,p,geometry,exportOptions(format,quality,maxLongEdge),resources);}catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在导出当前画面…"),"export");
}

QVariantMap PreviewController::adjustments() const {
    const auto& s=extra_settings_;
    return {{"filmExposureEv",s.film_exposure_ev},{"filmFormatMm",s.film_format_mm},
        {"grainActive",s.grain_active},{"grainAmount",s.grain_amount},
        {"halationActive",s.halation_active},{"halationAmount",s.halation_amount},
        {"glareActive",s.glare_active},{"glareAmount",s.glare_amount},
        {"yFilterShift",s.y_filter_shift},{"mFilterShift",s.m_filter_shift}};
}
void PreviewController::setAdjustment(const QString& name,const QVariant& value) {
    if(!settingsEditable())return;
    auto next=extra_settings_;
    if(name=="grainActive")next.grain_active=value.toBool();
    else if(name=="halationActive")next.halation_active=value.toBool();
    else if(name=="glareActive")next.glare_active=value.toBool();
    else {
        bool ok=false;const double v=value.toDouble(&ok);if(!ok||!std::isfinite(v))return;
        double* field=nullptr;double low=0,high=0;
        if(name=="filmExposureEv"){field=&next.film_exposure_ev;low=-8;high=8;}
        else if(name=="filmFormatMm"){field=&next.film_format_mm;low=4;high=200;}
        else if(name=="grainAmount"){field=&next.grain_amount;high=2;}
        else if(name=="halationAmount"){field=&next.halation_amount;high=4;}
        else if(name=="glareAmount"){field=&next.glare_amount;high=30;}
        else if(name=="yFilterShift"){field=&next.y_filter_shift;low=-1;high=1;}
        else if(name=="mFilterShift"){field=&next.m_filter_shift;low=-1;high=1;}
        else return;
        if(v<low||v>high)return;*field=v;
    }
    if(next==extra_settings_)return;
    extra_settings_=next;changed(name);
}
QVariantList PreviewController::libraryItems() const {
    QVariantList result;
    for(int i=0;i<int(library_.size());++i) {
        const auto& item=library_[i];
        result.push_back(QVariantMap{{"name",QFileInfo(item.url.toLocalFile()).fileName()},
            {"url",item.url},{"thumbnail",item.thumbnail},{"active",i==active_index_},
            {"selected",item.selected},{"error",item.error}});
    }
    return result;
}
int PreviewController::selectedCount() const {
    int count=0;for(const auto& item:library_)count+=item.selected;return count;
}
void PreviewController::saveActiveSettings(bool recordHistory) {
    if(active_index_<0||active_index_>=int(library_.size()))return;
    auto& item=library_[active_index_];const auto desired=settings();
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    // A saved pending edit has no rendered thumbnail yet. Do not let the
    // strip present an older look as if it represented the new settings.
    const bool changed=!sameSettings(item.settings,desired)||item.mode!=mode||item.geometry!=geometry_;
    if(changed){
        if(recordHistory)recordEdit(item,itemState(item));
        item.thumbnail={};item.pending_save=true;
    }
    item.settings=desired;item.mode=mode;
    item.geometry=geometry_;
    if(changed){scheduleSave();emit libraryChanged();emit historyChanged();}
}
void PreviewController::updateThumbnail() {
    if(active_index_<0||active_index_>=int(library_.size()))return;
    auto& item=library_[active_index_];
    item.thumbnail=dirty()?QImage{}:image_.scaled(180,120,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    item.error.clear();emit libraryChanged();
}
int PreviewController::addFile(const QUrl& url) {
    if(!url.isLocalFile())return -1;
    const QFileInfo info(url.toLocalFile());
    if(!info.isFile())return -1;
    const auto local=QUrl::fromLocalFile(info.canonicalFilePath());
    for(int i=0;i<int(library_.size());++i)
        if(library_[i].url.toLocalFile().compare(local.toLocalFile(),Qt::CaseInsensitive)==0)return i;
    LibraryItem item;item.url=local;item.settings=settings();
    item.mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    const auto stored=edit_store_.load(local,catalog_);
    item.save_blocked=stored.blocked;item.save_warning=stored.warning;
    if(stored.state){item.settings=stored.state->settings;item.mode=stored.state->mode;item.geometry=stored.state->geometry;item.has_stored=true;}
    library_.push_back(std::move(item));emit libraryChanged();return int(library_.size())-1;
}
void PreviewController::openFiles(const QVariantList& urls) {
    if(!ready_||busy_)return;
    saveActiveSettings();int first=-1;
    for(const auto& url:urls){const int index=addFile(url.toUrl());if(first<0&&index>=0)first=index;}
    if(first>=0)openItem(first);
}
void PreviewController::selectImage(int index) {
    if(!ready_||busy_||dialog_open_||index<0||index>=int(library_.size())||index==active_index_)return;
    saveActiveSettings();openItem(index);
}
void PreviewController::setItemSelected(int index,bool selected) {
    if(busy_||dialog_open_||index<0||index>=int(library_.size()))return;
    library_[index].selected=selected;emit libraryChanged();
}
void PreviewController::selectAllPhotos(bool selected) {
    if(busy_||dialog_open_)return;
    for(auto& item:library_)item.selected=selected;emit libraryChanged();
}
void PreviewController::syncSelectedSettings(bool includeCrop) {
    if(!frame_||busy_||dialog_open_)return;
    saveActiveSettings();int count=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected){
        auto& item=library_[i];auto next=currentState();
        if(!includeCrop)next.geometry=item.geometry;
        if(sameState(itemState(item),next))continue;
        // Each recipient owns its own undo history, just like its settings.
        item.undo.push_back(itemState(item));if(item.undo.size()>64)item.undo.erase(item.undo.begin());item.redo.clear();
        item.settings=next.settings;item.mode=next.mode;item.geometry=next.geometry;item.pending_save=true;
        item.thumbnail={};item.error.clear();++count;
    }
    status_=QStringLiteral("已将当前参数同步至 %1 张照片%2").arg(count)
        .arg(includeCrop?QStringLiteral("（含裁切）"):QStringLiteral("（各自保留裁切）"));
    scheduleSave();emit libraryChanged();emit historyChanged();emit stateChanged();scheduleThumbnails();
}
void PreviewController::applyFilmToSelected() {
    if(!frame_||busy_||dialog_open_)return;
    if(film_index_<0||std::size_t(film_index_)>=catalog_.films.size())return;
    saveActiveSettings();
    const auto& film=catalog_.films[std::size_t(film_index_)];
    int count=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected){
        auto& item=library_[i];
        if(item.settings.film_stock==film.id)continue;
        // Recipients keep their own exposure, paper, adjustments, decode mode
        // and crop; only the emulsion changes. Undo stays per photo, as sync.
        item.undo.push_back(itemState(item));if(item.undo.size()>64)item.undo.erase(item.undo.begin());item.redo.clear();
        item.settings.film_stock=film.id;item.pending_save=true;
        item.thumbnail={};item.error.clear();++count;
    }
    status_=QStringLiteral("已将胶片「%1」应用于 %2 张选中照片（其余参数各自保留）")
        .arg(text(film.label)).arg(count);
    scheduleSave();emit libraryChanged();emit historyChanged();emit stateChanged();scheduleThumbnails();
}
void PreviewController::applyPaperToSelected() {
    if(!frame_||busy_||dialog_open_)return;
    if(paper_index_<0||std::size_t(paper_index_)>=catalog_.papers.size())return;
    saveActiveSettings();
    const auto& paper=catalog_.papers[std::size_t(paper_index_)];
    const auto film_is_positive=[this](const std::string& id) {
        for(const auto& stock:catalog_.films)if(stock.id==id)return stock.is_positive;
        return false;
    };
    int count=0,slides=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected){
        auto& item=library_[i];
        // A slide is scanned directly and bypasses the paper stage; it keeps
        // its remembered paper for a later switch back to negative film.
        if(film_is_positive(item.settings.film_stock)){++slides;continue;}
        if(item.settings.print_stock==paper.id)continue;
        item.undo.push_back(itemState(item));if(item.undo.size()>64)item.undo.erase(item.undo.begin());item.redo.clear();
        item.settings.print_stock=paper.id;item.pending_save=true;
        item.thumbnail={};item.error.clear();++count;
    }
    status_=slides>0
        ?QStringLiteral("已将相纸「%1」应用于 %2 张选中照片（正片 %3 张跳过，其余参数各自保留）")
              .arg(text(paper.label)).arg(count).arg(slides)
        :QStringLiteral("已将相纸「%1」应用于 %2 张选中照片（其余参数各自保留）")
              .arg(text(paper.label)).arg(count);
    scheduleSave();emit libraryChanged();emit historyChanged();emit stateChanged();scheduleThumbnails();
}
void PreviewController::setCropRect(const QRectF& crop) {
    auto next=geometry_;next.crop=crop;setGeometry(next,"cropRect");
}
void PreviewController::setGeometry(const output::Geometry& geometry,const QString& editKey) {
    if(!frame_||busy_||dialog_open_||geometry==geometry_)return;
    try {
        auto next=visibleImage(frame_,full_image_,geometry);
        if(gesture_index_>=0&&gesture_key_!=editKey)endEditGesture();
        image_=std::move(next);geometry_=geometry;saveActiveSettings(true);updateThumbnail();
        failed_=false;emit geometryChanged();emit frameChanged();emit stateChanged();
    } catch(const std::exception& e){failed_=true;status_=text(e.what());emit stateChanged();}
}
void PreviewController::resetCrop(){setCropRect(QRectF(0,0,1,1));}
void PreviewController::setCropAspect(double ratio) {
    if(!frame_||!std::isfinite(ratio)||ratio<0)return;
    if(ratio==0){resetCrop();return;}
    // Ratios describe the displayed result, even after a quarter turn.
    if(geometry_.quarterTurns%2)ratio=1/ratio;
    const double original=double(frame_->width())/frame_->height();
    double w=1,h=1;
    if(original>ratio)w=ratio/original;else h=original/ratio;
    setCropRect(QRectF((1-w)/2,(1-h)/2,w,h));
}
void PreviewController::cancelExport(){if(cancel_batch_)cancel_batch_->store(true);}

editing::State PreviewController::itemState(const LibraryItem& item) {
    return {item.settings,item.mode,item.geometry};
}
editing::State PreviewController::currentState() const {
    return {settings(),decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16,geometry_};
}
bool PreviewController::canUndo() const {
    return settingsEditable()&&active_index_>=0&&!library_[active_index_].undo.empty();
}
bool PreviewController::canRedo() const {
    return settingsEditable()&&active_index_>=0&&!library_[active_index_].redo.empty();
}
void PreviewController::recordEdit(LibraryItem& item,const editing::State& before) {
    if(gesture_index_!=active_index_||!gesture_recorded_) {
        item.undo.push_back(before);
        if(item.undo.size()>64)item.undo.erase(item.undo.begin());
        item.redo.clear();
        if(gesture_index_==active_index_)gesture_recorded_=true;
    }
}
void PreviewController::beginEditGesture(const QString& key) {
    if(!settingsEditable()||active_index_<0||key.isEmpty())return;
    endEditGesture();gesture_index_=active_index_;gesture_key_=key;gesture_recorded_=false;gesture_redo_=library_[active_index_].redo;
    if(key=="straightenDegrees")auto_apply_.stop();
}
void PreviewController::endEditGesture() {
    const bool heldGeometry=gesture_key_=="straightenDegrees";
    if(gesture_index_>=0&&gesture_index_<int(library_.size())&&gesture_recorded_) {
        auto& item=library_[gesture_index_];
        // A drag returning to its starting value is not an undo step.
        if(!item.undo.empty()&&sameState(item.undo.back(),itemState(item))){item.undo.pop_back();item.redo=std::move(gesture_redo_);}
    }
    gesture_index_=-1;gesture_key_.clear();gesture_recorded_=false;gesture_redo_.clear();emit historyChanged();
    if(heldGeometry)schedulePreview();
}
void PreviewController::undo(){restoreHistory(false);}
void PreviewController::redo(){restoreHistory(true);}
void PreviewController::restoreHistory(bool redo) {
    if(!frame_||!settingsEditable()||active_index_<0)return;
    endEditGesture();
    auto& item=library_[active_index_];auto& from=redo?item.redo:item.undo;
    auto& to=redo?item.undo:item.redo;
    if(from.empty())return;
    const auto target=from.back();
    try {
        // Prepare geometry before moving the history cursor. Allocation or
        // validation failure leaves the frame and both stacks intact.
        const bool geometryChangedNow=target.geometry!=geometry_;
        auto next=geometryChangedNow?visibleImage(frame_,full_image_,target.geometry):image_;
        to.push_back(currentState());if(to.size()>64)to.erase(to.begin());from.pop_back();
        geometry_=target.geometry;image_=std::move(next);
        loadSettings(target.settings,target.mode);saveActiveSettings();
        ++settings_revision_;auto_apply_.stop();schedulePreview();updateThumbnail();
        failed_=false;status_=redo?QStringLiteral("已重做当前照片调整"):QStringLiteral("已撤销当前照片调整");
        if(geometryChangedNow){emit geometryChanged();emit frameChanged();}
        emit settingsChanged();emit stateChanged();emit historyChanged();
    }catch(const std::exception& e){failed_=true;status_=text(e.what());emit stateChanged();}
}
void PreviewController::scheduleSave() {
    save_timer_.start();emit stateChanged();
}
void PreviewController::flushEdits() {
    save_timer_.stop();
    for(auto& item:library_)if(item.pending_save&&!item.save_blocked) {
        try {edit_store_.save(item.url,itemState(item));item.pending_save=false;item.has_stored=true;item.save_warning.clear();}
        catch(const std::exception& e){item.save_warning=text(e.what());}
    }
    emit stateChanged();
}
QString PreviewController::persistenceWarning() const {
    QStringList warnings;
    for(const auto& item:library_)if(!item.save_warning.isEmpty())
        warnings.append(QFileInfo(item.url.toLocalFile()).fileName()+": "+item.save_warning);
    return warnings.join("\n");
}
QString PreviewController::savedStatus() const {
    if(!persistenceWarning().isEmpty())return QStringLiteral("部分设置未保存，请查看下方提示");
    for(const auto& item:library_)if(item.pending_save)return QStringLiteral("正在自动保存设置…");
    if(active_index_>=0&&library_[active_index_].has_stored)return QStringLiteral("当前照片设置已自动保存");
    return QStringLiteral("调整将自动保存，重新打开照片时恢复");
}
// Flips are stored in output axes. Rotating the displayed image also swaps
// those axes; without this a mirrored photo would turn the opposite way.
void PreviewController::rotateClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+1)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::rotateCounterClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+3)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::setFlipHorizontal(bool value){auto next=geometry_;next.flipHorizontal=value;setGeometry(next,"flipHorizontal");}
void PreviewController::setFlipVertical(bool value){auto next=geometry_;next.flipVertical=value;setGeometry(next,"flipVertical");}
void PreviewController::setStraightenDegrees(double value){auto next=geometry_;next.straightenDegrees=value;setGeometry(next,"straightenDegrees");}
void PreviewController::resetGeometry(){setGeometry(output::Geometry{});}

void PreviewController::exportBatch(const QUrl& folder,int format,int quality,int maxLongEdge) {
    if(!ready_||busy_||!folder.isLocalFile()||!selectedCount())return;
    saveActiveSettings();
    std::vector<LibraryItem> items;
    for(const auto& item:library_)if(item.selected)items.push_back(item);
    // Strip thumbnails from the job snapshot. Only one developed frame is
    // retained by the worker, regardless of the number of imported photos.
    for(auto& item:items)item.thumbnail={};
    const auto held=frame_;const auto resources=resources_;const auto destination=path(folder);
    const auto gen=++generation_;cancel_batch_=std::make_shared<std::atomic_bool>(false);
    const auto cancel=cancel_batch_;batch_progress_=0;batch_total_=int(items.size());
    submit([this,items=std::move(items),held,resources,destination,gen,cancel,format,quality,maxLongEdge](auto& host){
        Reply r;r.generation=gen;r.operation="batch-export";
        int done=0,skipped=0,errors=0,processed=0;QStringList details;
        try {
            const auto options=exportOptions(format,quality,maxLongEdge);
            if(!std::filesystem::is_directory(destination))throw std::runtime_error("Export destination is not a folder");
            const QString extension=format==0?".tif":format==3?".jpg":".png";
            QSet<QString> names;
            for(const auto& item:items) {
                if(cancel->load())break;
                const auto source=path(item.url);
                const QString stem=QFileInfo(item.url.toLocalFile()).completeBaseName();
                QString name=stem+extension;int suffix=2;
                while(names.contains(name.toCaseFolded()))name=stem+"-"+QString::number(suffix++)+extension;
                names.insert(name.toCaseFolded());
                const auto target=destination/std::filesystem::path(name.toStdWString());
                try {
                    if(std::filesystem::exists(target))++skipped;
                    else {
                        const bool useShown=held&&held->source==source&&held->decode_mode==item.mode&&sameSettings(held->settings,item.settings);
                        const auto developed=useShown?held:host.open_raw(source,item.mode,item.settings);
                        output::exportFrame(developed,target,item.geometry,options,resources);++done;
                    }
                }catch(const std::exception& e){++errors;details.append(name+": "+text(e.what()));}
                ++processed;
                QMetaObject::invokeMethod(this,[this,gen,processed]{if(generation_==gen){batch_progress_=processed;emit stateChanged();}},Qt::QueuedConnection);
            }
            r.message=QStringLiteral("%1：导出 %2 张，跳过已有 %3 张，失败 %4 张")
                .arg(cancel->load()?QStringLiteral("批量导出已取消"):QStringLiteral("批量导出完成")).arg(done).arg(skipped).arg(errors);
            if(errors)r.error=r.message+"\n"+details.join("\n");
        }catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },QStringLiteral("正在批量导出…"),"batch-export");
}

void PreviewController::openEffectsGallery() {
    if(!frame_||busy_||dialog_open_)return;
    if(film_index_<0||std::size_t(film_index_)>=catalog_.films.size())return;
    saveActiveSettings();
    const auto shown=frame_;const auto base=settings();
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    const int baseIndex=film_index_;
    // Placeholder cells keep the grid in catalog order while live-tier
    // renders stream in; each delivery fills one cell and bumps the progress.
    gallery_films_.clear();
    for(std::size_t i=0;i<catalog_.films.size();++i) {
        const auto& film=catalog_.films[i];
        gallery_films_.push_back(QVariantMap{{"id",text(film.id)},{"name",text(film.label)},
            {"positive",film.is_positive},{"catalogIndex",int(i)},{"ready",false},{"image",QImage()}});
    }
    gallery_progress_=0;gallery_total_=int(catalog_.films.size());
    emit galleryChanged();emit stateChanged();
    // The current film renders last so the session ends back on the base
    // parameters without paying one extra negative invalidation.
    std::vector<std::pair<int,std::string>> order;
    for(std::size_t i=0;i<catalog_.films.size();++i)
        if(int(i)!=baseIndex)order.emplace_back(int(i),catalog_.films[i].id);
    order.emplace_back(baseIndex,catalog_.films[std::size_t(baseIndex)].id);
    const auto gen=generation_;
    cancel_gallery_=std::make_shared<std::atomic_bool>(false);
    const auto cancel=cancel_gallery_;
    submit([this,shown,base,mode,order,gen,cancel](auto& host){
        Reply r;r.generation=gen;r.operation="effects-gallery";
        int done=0;
        try {
            // Same reconciliation as apply(): variants need the session to
            // hold the photo the user is looking at.
            const auto active=host.current();
            if(!active||active->source!=shown->source||active->decode_mode!=mode)
                host.open_raw(shown->source,mode,base);
            for(const auto& [index,id]:order) {
                if(cancel->load())break;
                auto cellSettings=base;cellSettings.film_stock=id;
                // Full-tier reprints: the engine's sub-full tiers render with
                // crushed G/B on Windows Vulkan. Each cell is scaled to the
                // comparison view's size host-side (~4 MB/film retained).
                const auto variant=host.render_variant(cellSettings,"full");
                const auto image=imageFromFrame(variant)
                    .scaled(1600,1600,Qt::KeepAspectRatio,Qt::SmoothTransformation)
                    .copy();
                QMetaObject::invokeMethod(this,[this,gen,index,image]{
                    if(generation_!=gen)return;
                    auto cell=gallery_films_.value(index).toMap();
                    cell.insert("image",image);cell.insert("ready",true);
                    gallery_films_[index]=cell;++gallery_progress_;
                    emit galleryChanged();emit stateChanged();
                },Qt::QueuedConnection);
                ++done;
            }
            r.message=(cancel->load()?QStringLiteral("效果总览已取消（已生成 %1/%2）")
                                     :QStringLiteral("效果总览已生成（%1/%2）"))
                          .arg(done).arg(order.size());
        }catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },QStringLiteral("正在生成效果总览…"),"effects-gallery");
}
void PreviewController::cancelEffectsGallery(){if(cancel_gallery_)cancel_gallery_->store(true);}

void PreviewController::balanceBrightness() {
    if(!frame_||busy_||dialog_open_)return;
    saveActiveSettings();
    // Recipients are selected photos other than the active reference; slides
    // bypass the paper stage, so print exposure cannot move their brightness.
    struct Target { int index; QUrl url; desktop::RenderSettings settings; desktop::DecodeMode mode; };
    std::vector<Target> targets;
    int slides=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected){
        const auto& item=library_[i];
        bool slide=false;
        for(const auto& stock:catalog_.films)if(stock.id==item.settings.film_stock)slide=stock.is_positive;
        if(slide){++slides;continue;}
        targets.push_back({i,item.url,item.settings,item.mode});
    }
    if(targets.empty()&&slides==0)return;
    const auto shown=frame_;const auto base=settings();
    const auto shownMode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    const QString referenceName=fileName();
    balance_progress_=0;balance_total_=int(targets.size());
    cancel_balance_=std::make_shared<std::atomic_bool>(false);
    const auto cancel=cancel_balance_;
    const auto gen=generation_;
    submit([this,shown,base,shownMode,targets=std::move(targets),slides,referenceName,gen,cancel](auto& host){
        Reply r;r.generation=gen;r.operation="balance-brightness";
        int done=0,limited=0;QStringList failed;
        try {
            // The reference is the active photo rendered at its desired
            // settings at the full tier — exactly what the user is looking
            // at and likes. Recipients are opened fresh and iterated at the
            // full tier as well, bypassing the session cache: upstream
            // v2.4.0 carries cross-session print state between a live
            // variant and a restored session, which would render the
            // recipient in the reference's look. A fresh spk_open is immune.
            const auto active=host.current();
            const auto reference=(active&&active->source==shown->source&&active->decode_mode==shownMode)
                ?host.render(base):host.open_raw(shown->source,shownMode,base);
            const double referenceLuma=medianLuma(imageFromFrame(reference));
            for(const auto& target:targets) {
                if(cancel->load())break;
                const auto source=path(target.url);
                try {
                    auto settings=target.settings;
                    const auto frame=host.open_raw(source,target.mode,settings,"full",false);
                    double luma=medianLuma(imageFromFrame(frame));
                    double delta=0;
                    // The paper curve's response to print exposure is not
                    // linear in luma; each iteration multiplies the measured
                    // ratio toward 1. Full-tier reprints (~230 ms each)
                    // absorb even a slow curve within eight rounds.
                    for(int iteration=0;iteration<8;++iteration) {
                        delta=std::log2(std::max(1e-9,referenceLuma)/std::max(1e-9,luma));
                        if(std::abs(delta)<0.05)break;
                        auto next=settings;
                        next.print_exposure=std::exp2(-std::clamp(-std::log2(settings.print_exposure)+delta,-2.0,2.0));
                        if(next==settings)break;
                        settings=std::move(next);
                        luma=medianLuma(imageFromFrame(host.render(settings)));
                    }
                    if(std::abs(delta)>=0.05)++limited;  // clamped at the slider range or not yet converged
                    const auto finalFrame=host.render(settings);
                    const auto thumbnail=imageFromFrame(finalFrame)
                        .scaled(180,120,Qt::KeepAspectRatio,Qt::SmoothTransformation);
                    const double exposure=settings.print_exposure;
                    QMetaObject::invokeMethod(this,[this,gen,index=target.index,exposure,thumbnail]{
                        if(generation_!=gen)return;
                        if(index>=0&&index<int(library_.size())) {
                            auto& item=library_[index];
                            item.undo.push_back(itemState(item));
                            if(item.undo.size()>64)item.undo.erase(item.undo.begin());
                            item.redo.clear();
                            item.settings.print_exposure=exposure;
                            item.pending_save=true;item.thumbnail=thumbnail;item.error.clear();
                        }
                        ++balance_progress_;
                        emit libraryChanged();emit historyChanged();emit stateChanged();
                    },Qt::QueuedConnection);
                    ++done;
                }catch(const std::exception& e){
                    failed.append(QFileInfo(target.url.toLocalFile()).fileName()+": "+text(e.what()));
                }
            }
            r.message=(cancel->load()?QStringLiteral("亮度均衡已取消：调整 %1/%2 张，正片跳过 %3 张，未完全收敛 %4 张")
                                     :QStringLiteral("亮度已均衡至「%5」：调整 %1/%2 张，正片跳过 %3 张，未完全收敛 %4 张"))
                          .arg(done).arg(targets.size()).arg(slides).arg(limited);
            if(!cancel->load())r.message=r.message.arg(referenceName);
            if(!failed.isEmpty())r.message+=QStringLiteral("\n失败：")+failed.join(QStringLiteral("；"));
            if(!failed.isEmpty())r.error=r.message;
        }catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },QStringLiteral("正在均衡亮度…"),"balance-brightness");
}
void PreviewController::cancelBalance(){if(cancel_balance_)cancel_balance_->store(true);}

void PreviewController::scheduleThumbnails() {
    if(!ready_||stopping_)return;
    bool added=false;
    for(int i=0;i<int(library_.size());++i) {
        const auto& item=library_[i];
        if(!item.thumbnail.isNull()||!item.error.isEmpty())continue;
        if(i==active_index_)continue;  // the main worker renders it on demand
        const auto job=item;  // snapshot: a later edit supersedes the queued thumb
        submitThumb([this,gen=generation_,index=i,url=job.url,mode=job.mode,
                     settings=job.settings](auto& host){
            QImage thumbnail;
            QString error;
            try {
                const auto frame=host.open_raw(path(url),mode,settings,"full",false);
                thumbnail=imageFromFrame(frame).scaled(180,120,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            }catch(const std::exception& e){error=text(e.what());}
            QMetaObject::invokeMethod(this,[this,gen,index,mode,settings,thumbnail,error]{
                if(generation_!=gen)return;
                if(index>=0&&index<int(library_.size())) {
                    auto& item=library_[index];
                    const bool current=item.mode==mode&&sameSettings(item.settings,settings);
                    if(current&&item.thumbnail.isNull()) {
                        if(error.isEmpty())item.thumbnail=thumbnail;
                        else item.error=error;
                    }
                    // A stale or superseded job only retires its pending count.
                }
                --thumbs_pending_;
                emit libraryChanged();
            },Qt::QueuedConnection);
            return Reply{};
        });
        ++thumbs_pending_;
        added=true;
    }
    if(added)emit libraryChanged();
}

void PreviewController::submitThumb(Job job) {
    // Thumbnails ride the user worker's low-priority queue: one engine, one
    // 50 MP session at a time, and every user operation runs first.
    {std::lock_guard lock(mutex_);low_jobs_.push_back(std::move(job));}
    wake_.notify_one();
}
