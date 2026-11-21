// A small headless host for the Linux plugins: loads the built .clap and the .vst3 bundle's object, plays a note
// through each, saves and loads a state, and (where a display is there) opens the CLAP editor in a window of its own
// and pumps it a few times. Not a validator: a check that what is built loads and plays.

#include <clap/clap.h>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// --- CLAP ----------------------------------------------------------------------------------------------------------

struct ClapHost {
    clap_host_t host{};
    clap_host_timer_support_t timer{};
    const clap_plugin_t *plugin = nullptr;
    clap_plugin_timer_support_t *pluginTimer = nullptr;
    std::atomic<bool> callback{false};
    clap_id timerId = CLAP_INVALID_ID;

    ClapHost() {
        host.clap_version = CLAP_VERSION_INIT;
        host.host_data = this;
        host.name = "fmvibe-linux-host";
        host.vendor = "FMVibe";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host_t *h, const char *id) -> const void * {
            auto *self = static_cast<ClapHost *>(h->host_data);
            if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
                return &self->timer;
            return nullptr;
        };
        host.request_restart = [](const clap_host_t *) {};
        host.request_process = [](const clap_host_t *) {};
        host.request_callback = [](const clap_host_t *h) {
            static_cast<ClapHost *>(h->host_data)->callback.store(true);
        };
        timer.register_timer = [](const clap_host_t *h, uint32_t, clap_id *id) {
            auto *self = static_cast<ClapHost *>(h->host_data);
            self->timerId = 1;
            *id = 1;
            return true;
        };
        timer.unregister_timer = [](const clap_host_t *h, clap_id) {
            static_cast<ClapHost *>(h->host_data)->timerId = CLAP_INVALID_ID;
            return true;
        };
    }
};

struct Events {
    std::vector<clap_event_note_t> notes;
    clap_input_events_t in{};
    uint32_t at = 0;

    Events() {
        in.ctx = this;
        in.size = [](const clap_input_events_t *list) {
            return static_cast<uint32_t>(static_cast<Events *>(list->ctx)->notes.size());
        };
        in.get = [](const clap_input_events_t *list, uint32_t index) -> const clap_event_header_t * {
            auto *self = static_cast<Events *>(list->ctx);
            return index < self->notes.size() ? &self->notes[index].header : nullptr;
        };
    }

    void note(bool on, int key, double velocity) {
        clap_event_note_t event{};
        event.header.size = sizeof event;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        event.header.time = 0;
        event.note_id = -1;
        event.port_index = 0;
        event.channel = 0;
        event.key = static_cast<int16_t>(key);
        event.velocity = velocity;
        notes.push_back(event);
    }
};

struct State {
    std::vector<uint8_t> bytes;
    clap_ostream_t out{};
    clap_istream_t in{};
    size_t at = 0;

    State() {
        out.ctx = this;
        out.write = [](const clap_ostream_t *stream, const void *buffer, uint64_t size) -> int64_t {
            auto *self = static_cast<State *>(stream->ctx);
            const auto *bytes = static_cast<const uint8_t *>(buffer);
            self->bytes.insert(self->bytes.end(), bytes, bytes + size);
            return static_cast<int64_t>(size);
        };
        in.ctx = this;
        in.read = [](const clap_istream_t *stream, void *buffer, uint64_t size) -> int64_t {
            auto *self = static_cast<State *>(stream->ctx);
            const size_t left = self->bytes.size() - std::min(self->at, self->bytes.size());
            const size_t take = static_cast<size_t>(std::min<uint64_t>(size, left));
            std::memcpy(buffer, self->bytes.data() + self->at, take);
            self->at += take;
            return static_cast<int64_t>(take);
        };
    }
};

double play(const clap_plugin_t *plugin, int key = 69, int blocks = 8) {
    Events events;
    events.note(true, key, 1.0);
    std::vector<float> left(512), right(512);
    float peak = 0.0f;
    for (int block = 0; block < blocks; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        float *channels[2] = {left.data(), right.data()};
        clap_audio_buffer_t audio{};
        audio.data32 = channels;
        audio.channel_count = 2;
        clap_process_t process{};
        process.frames_count = 512;
        process.steady_time = block * 512;
        process.audio_outputs = &audio;
        process.audio_outputs_count = 1;
        process.in_events = &events.in;
        process.out_events = nullptr;
        if (block == 1)
            events.note(false, key, 0.0);
        plugin->process(plugin, &process);
        for (float sample : left)
            peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

void testClap(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        check(false, "the .clap opens");
        std::printf("       %s\n", dlerror());
        return;
    }
    check(true, "the .clap opens");
    const auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(library, "clap_entry"));
    if (!entry || !entry->init("")) {
        check(false, "its entry takes");
        return;
    }
    const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "one plugin in its factory");
    if (!factory)
        return;
    ClapHost host;
    const clap_plugin_descriptor_t *descriptor = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t *plugin = factory->create_plugin(factory, &host.host, descriptor->id);
    check(plugin != nullptr, "the plugin is made");
    if (!plugin)
        return;
    host.plugin = plugin;
    check(plugin->init(plugin), "it initializes");
    check(plugin->activate(plugin, 48000.0, 1, 512), "it activates");
    check(plugin->start_processing(plugin), "it starts processing");

    const auto *params = static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    check(params && params->count(plugin) > 1000, "hosts are given its params");
    const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(gui != nullptr, "it has an editor");

    const double peak = play(plugin);
    check(peak > 0.001, "a note sounds");
    std::printf("       peak %.3f\n", peak);

    State saved;
    const auto *state = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    check(state && state->save(plugin, &saved.out) && saved.bytes.size() > 1000, "its state saves");
    State loaded;
    loaded.bytes = saved.bytes;
    check(state && state->load(plugin, &loaded.in), "its state loads back");

    if (gui && std::getenv("DISPLAY")) {
        check(gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false), "its editor takes an X11 window");
        check(gui->create(plugin, CLAP_WINDOW_API_X11, false), "its editor is made");
        uint32_t width = 0, height = 0;
        check(gui->get_size(plugin, &width, &height) && width > 100 && height > 100, "it has a size");
        Display *display = XOpenDisplay(nullptr);
        const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1300, 1080, 0, 0, 0);
        XStoreName(display, parent, "FMVibe editor host");
        XMapWindow(display, parent);
        XSync(display, False);
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_X11;
        window.x11 = parent;
        check(gui->set_parent(plugin, &window), "it goes into a host's window");
        check(gui->show(plugin), "it shows");
        host.pluginTimer = static_cast<clap_plugin_timer_support_t *>(
            const_cast<void *>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT)));
        for (int frame = 0; frame < 8; ++frame) {
            if (host.pluginTimer)
                host.pluginTimer->on_timer(plugin, 1);
            if (host.callback.exchange(false))
                plugin->on_main_thread(plugin);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        check(host.timerId != CLAP_INVALID_ID, "its editor took the host's timer");
        // The editor's child window has painted: its pixels aren't one flat color (the view's own drawing shows).
        Window root = None, above = None, child = None, *children = nullptr;
        unsigned count = 0;
        if (XQueryTree(display, parent, &root, &above, &children, &count) && children) {
            if (count > 0)
                child = children[count - 1]; // the topmost child: the editor's window
            XFree(children);
        }
        bool painted = false;
        if (child) {
            XWindowAttributes attributes{};
            XGetWindowAttributes(display, child, &attributes);
            XSync(display, False);
            if (XImage *image = XGetImage(display, child, 0, 0, 200, 200, AllPlanes, ZPixmap)) {
                const unsigned long first = XGetPixel(image, 0, 0);
                for (int y = 0; y < 200 && !painted; y += 4)
                    for (int x = 0; x < 200 && !painted; x += 4)
                        painted = XGetPixel(image, x, y) != first;
                XDestroyImage(image);
            }
            // FMVIBE_SHOT=file.ppm saves the whole editor: a look at what the host's window shows.
            if (const char *shot = std::getenv("FMVIBE_SHOT")) {
                const int shotWidth = std::min(attributes.width, 900), shotHeight = std::min(attributes.height, 700);
                if (XImage *image = XGetImage(display, child, 0, 0, static_cast<unsigned>(shotWidth),
                                              static_cast<unsigned>(shotHeight), AllPlanes, ZPixmap)) {
                    if (FILE *file = std::fopen(shot, "wb")) {
                        std::fprintf(file, "P6\n%d %d\n255\n", shotWidth, shotHeight);
                        for (int y = 0; y < shotHeight; ++y)
                            for (int x = 0; x < shotWidth; ++x) {
                                const unsigned long pixel = XGetPixel(image, x, y);
                                const unsigned char rgb[3] = {static_cast<unsigned char>((pixel >> 16) & 0xFF),
                                                              static_cast<unsigned char>((pixel >> 8) & 0xFF),
                                                              static_cast<unsigned char>(pixel & 0xFF)};
                                std::fwrite(rgb, 1, 3, file);
                            }
                        std::fclose(file);
                    }
                    XDestroyImage(image);
                }
            }
        }
        check(painted, "its editor paints");
        gui->hide(plugin);
        gui->destroy(plugin);
        XDestroyWindow(display, parent);
        XCloseDisplay(display);
    }

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    check(true, "it closes");
    dlclose(library);
}

// --- VST3 ----------------------------------------------------------------------------------------------------------

using namespace Steinberg;
using namespace Steinberg::Vst;

// What a Linux host gives a VST3 editor: a run loop with the display's socket and a timer, which here is fired by
// hand. The editor is pumped by that timer, so this checks the path Bitwig takes.
class TestRunLoop final : public Steinberg::Linux::IRunLoop {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Steinberg::Linux::IRunLoop)
        QUERY_INTERFACE(iid, obj, Steinberg::Linux::IRunLoop::iid, Steinberg::Linux::IRunLoop)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override { return --refs_; } // one reference, for the whole test
    tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                            Steinberg::Linux::FileDescriptor) override {
        event_ = handler;
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler) override {
        if (event_ == handler)
            event_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler, uint64_t) override {
        timer_ = handler;
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler) override {
        if (timer_ == handler)
            timer_ = nullptr;
        return kResultOk;
    }
    bool tick() {
        if (!timer_)
            return false;
        timer_->onTimer();
        return true;
    }

private:
    std::atomic<uint32> refs_{1};
    Steinberg::Linux::IEventHandler *event_ = nullptr;
    Steinberg::Linux::ITimerHandler *timer_ = nullptr;
};

class TestFrame final : public IPlugFrame {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
        if (FUnknownPrivate::iidEqual(iid, Steinberg::Linux::IRunLoop::iid)) {
            runLoop.addRef();
            *obj = &runLoop;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override { return --refs_; }
    tresult PLUGIN_API resizeView(IPlugView *, ViewRect *) override { return kResultOk; }

    TestRunLoop runLoop;

private:
    std::atomic<uint32> refs_{1};
};

void testVst3(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        check(false, "the VST3 object opens");
        std::printf("       %s\n", dlerror());
        return;
    }
    check(true, "the VST3 object opens");
    using GetFactory = IPluginFactory *(*)();
    const auto getFactory = reinterpret_cast<GetFactory>(dlsym(library, "GetPluginFactory"));
    IPluginFactory *factory = getFactory ? getFactory() : nullptr;
    check(factory != nullptr, "its factory is there");
    if (!factory) {
        dlclose(library);
        return;
    }
    PClassInfo info{};
    check(factory->getClassInfo(0, &info) == kResultOk && factory->countClasses() == 1, "one class in its factory");
    // What a strict scanner reads before it loads anything: the class' name, category and ids, in both forms.
    PClassInfo2 info2{};
    PClassInfoW infoW{};
    IPluginFactory2 *factory2 = nullptr;
    IPluginFactory3 *factory3 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&factory2));
    factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void **>(&factory3));
    const bool forms = factory2 && factory3 && factory2->getClassInfo2(0, &info2) == kResultOk &&
                       factory3->getClassInfoUnicode(0, &infoW) == kResultOk;
    check(forms, "its class reads in both forms");
    if (factory2)
        factory2->release();
    if (factory3)
        factory3->release();
    check(std::strcmp(info.category, kVstAudioEffectClass) == 0 && info.name[0] != 0 && info2.vendor[0] != 0 &&
              infoW.name[0] != 0 && std::memcmp(info.cid, info2.cid, sizeof(TUID)) == 0,
          "its class says what it is");
    FUnknown *unknown = nullptr;
    check(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void **>(&unknown)) == kResultOk,
          "the component is made");
    auto *component = static_cast<IComponent *>(unknown);
    if (!component) {
        dlclose(library);
        return;
    }
    check(component->initialize(nullptr) == kResultOk, "it initializes");
    IAudioProcessor *processor = nullptr;
    check(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void **>(&processor)) == kResultOk,
          "it is a processor");
    if (processor) {
        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 512;
        setup.sampleRate = 48000.0;
        check(processor->setupProcessing(setup) == kResultOk, "it sets up");
        check(component->setActive(true) == kResultOk, "it activates");
        check(processor->setProcessing(true) == kResultOk, "it processes");
        std::vector<float> left(512), right(512);
        float *channels[2] = {left.data(), right.data()};
        AudioBusBuffers output{};
        output.numChannels = 2;
        output.channelBuffers32 = channels;
        ProcessData data{};
        data.symbolicSampleSize = kSample32;
        data.numSamples = 512;
        data.numOutputs = 1;
        data.outputs = &output;
        data.processContext = nullptr;
        float peak = 0.0f;
        for (int block = 0; block < 4; ++block) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            processor->process(data);
            for (float sample : left)
                peak = std::max(peak, std::fabs(sample));
        }
        check(peak >= 0.0f, "a block goes through"); // silence without a note: the check is that it doesn't crash
        processor->setProcessing(false);

        // The editor, as Bitwig opens it: a view in an X11 host window, pumped by the run loop's timer.
        IEditController *edit = nullptr;
        component->queryInterface(IEditController::iid, reinterpret_cast<void **>(&edit));
        IPlugView *view = edit ? edit->createView(ViewType::kEditor) : nullptr;
        check(view != nullptr, "its VST3 editor view is made");
        if (view && std::getenv("DISPLAY")) {
            TestFrame frame;
            check(view->setFrame(&frame) == kResultOk, "the host's frame takes");
            check(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue,
                  "the view takes an X11 window");
            Display *display = XOpenDisplay(nullptr);
            const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1300, 1080, 0, 0, 0);
            XMapWindow(display, parent);
            XSync(display, False);
            check(view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(parent)),
                                 kPlatformTypeX11EmbedWindowID) == kResultOk,
                  "the view goes into the host's window");
            bool pumped = false;
            for (int tick = 0; tick < 8; ++tick) {
                pumped = frame.runLoop.tick() || pumped;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            check(pumped, "the host's run loop pumps the editor");
            Window root = None, above = None, child = None, *children = nullptr;
            unsigned count = 0;
            if (XQueryTree(display, parent, &root, &above, &children, &count) && children) {
                if (count > 0)
                    child = children[count - 1];
                XFree(children);
            }
            bool painted = false;
            if (child) {
                XSync(display, False);
                if (XImage *image = XGetImage(display, child, 0, 0, 200, 200, AllPlanes, ZPixmap)) {
                    const unsigned long first = XGetPixel(image, 0, 0);
                    for (int y = 0; y < 200 && !painted; y += 4)
                        for (int x = 0; x < 200 && !painted; x += 4)
                            painted = XGetPixel(image, x, y) != first;
                    XDestroyImage(image);
                }
            }
            check(painted, "the VST3 editor paints");
            view->removed();
            XDestroyWindow(display, parent);
            XCloseDisplay(display);
        }
        if (view) {
            view->setFrame(nullptr);
            view->release();
        }
        if (edit)
            edit->release();
        component->setActive(false);
        processor->release();
    }
    check(component->terminate() == kResultOk, "it terminates");
    component->release();
    dlclose(library);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <FMVibe.clap> <FMVibe.so>\n", argv[0]);
        return 2;
    }
    std::printf("== CLAP\n");
    testClap(argv[1]);
    std::printf("== VST3\n");
    testVst3(argv[2]);
    std::printf(failures == 0 ? "all checks passed\n" : "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
