#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include <pango/pangocairo.h>
#include <unistd.h>

#include "LuaPluginRuntime.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void writePlugin(const std::filesystem::path& directory, const std::string& manifest, const std::string& script) {
    std::filesystem::create_directories(directory);
    std::ofstream ini(directory / "plugin.ini", std::ios::binary);
    ini << manifest;
    std::ofstream main(directory / "main.lua", std::ios::binary);
    main << script;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("xournalpp-gtk4-lua-plugins-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    const auto xdgConfigRoot = root / "xdg-config";
    const auto xdgDataRoot = root / "xdg-data";
    const auto xdgStateRoot = root / "xdg-state";
#ifdef _WIN32
    const auto expectedStateRoot = xdgDataRoot;
#else
    const auto expectedStateRoot = xdgStateRoot;
#endif
    const auto xdgConfigString = xdgConfigRoot.string();
    const auto xdgDataString = xdgDataRoot.string();
    const auto xdgStateString = xdgStateRoot.string();
    require(g_setenv("XDG_CONFIG_HOME", xdgConfigString.c_str(), TRUE) &&
                    g_setenv("XDG_DATA_HOME", xdgDataString.c_str(), TRUE) &&
                    g_setenv("XDG_STATE_HOME", xdgStateString.c_str(), TRUE),
            "Could not isolate the XDG roots for plugin folder tests");

    std::vector<std::string> systemFontFamilies;
    if (auto* fontMap = pango_cairo_font_map_get_default()) {
        PangoFontFamily** families = nullptr;
        int familyCount = 0;
        pango_font_map_list_families(fontMap, &families, &familyCount);
        for (int index = 0; index < familyCount; ++index) {
            if (const char* name = pango_font_family_get_name(families[index])) {
                systemFontFamilies.emplace_back(name);
            }
        }
        g_free(families);
    }
    require(!systemFontFamilies.empty(), "Pango did not report any system fonts for the font API tests");

    const auto colorCycle = root / "ColorCycle";
    writePlugin(colorCycle,
                "[about]\nauthor=Test author\ndescription=Color cycle test\nversion=1\n"
                "[default]\nenabled=false\n[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Cycle', callback='cycle', mode=7, "
                "parentPath='Tools/Test', accelerator='<Control>t', shortcut='<Shift>r'})\n"
                "end\n"
                "function cycle(mode)\n"
                "  app.changeToolColor({color=0x123456, tool='pen', selection=true})\n"
                "  app.changeActionState('test-mode', mode)\n"
                "end\n");
    const auto defaultPlugin = root / "DefaultPlugin";
    writePlugin(defaultPlugin,
                "[about]\nauthor=Test author\ndescription=Default enabled\nversion=1\n"
                "[default]\nenabled=true\n[plugin]\nmainfile=main.lua\n",
                "value = 42\n");
    const auto invalidApi = root / "InvalidApi";
    writePlugin(invalidApi,
                "[about]\ndescription=Invalid API values\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Broken', callback='broken', mode='not-an-integer'})\n"
                "end\n");
    const auto unavailableAction = root / "UnavailableAction";
    writePlugin(unavailableAction,
                "[about]\ndescription=Unavailable host action\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi() app.registerUi({menu='Unavailable', callback='run'}) end\n"
                "function run() app.activateAction('missing-action') end\n");
    const auto dialogPlugin = root / "DialogPlugin";
    writePlugin(dialogPlugin,
                "[about]\ndescription=Asynchronous plugin dialog\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Dialog', callback='showDialog'})\n"
                "  app.registerUi({menu='Error Dialog', callback='showErrorDialog'})\n"
                "end\n"
                "function showDialog() app.openDialog('Choose one', {'First', 'Second'}, 'dialogResult') end\n"
                "function showErrorDialog() app.openDialog('Problem', {'OK'}, '', true) end\n"
                "function dialogResult(result) app.changeActionState('test-mode', result) end\n");
    const auto backgroundPlugin = root / "BackgroundPlugin";
    writePlugin(backgroundPlugin,
                "[about]\ndescription=Page background action\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi() app.registerUi({menu='Set paper', callback='setPaper'}) end\n"
                "function setPaper() app.changeCurrentPageBackground('graph', 'r1=14.17') end\n");
    const auto beamerPlugin = root / "BeamerPresentation";
    writePlugin(beamerPlugin,
                "[about]\ndescription=Beamer presentation controls\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/BeamerPresentation/main.lua"));
    const auto layerActionsPlugin = root / "LayerActions";
    writePlugin(layerActionsPlugin,
                "[about]\ndescription=Layer operations\n[default]\nenabled=false\n"
                "[plugin]\nmainfile=main.lua\n",
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/LayerActions/main.lua"));
    const auto strokeApiPlugin = root / "StrokeApiPlugin";
    writePlugin(strokeApiPlugin,
                "[about]\ndescription=Read editable strokes\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read strokes', callback='readStrokes'})\n"
                "  app.registerUi({menu='Read selection', callback='readSelection'})\n"
                "end\n"
                "function readStrokes()\n"
                "  local strokes = app.getStrokes('all')\n"
                "  assert(#strokes == 2)\n"
                "  assert(strokes[1].x[2] == 12.5 and strokes[1].y[2] == 24.5)\n"
                "  assert(strokes[1].pressure[2] == 0.75 and strokes[1].tool == 'pen')\n"
                "  assert(strokes[1].width == 2.25 and strokes[1].color == 0x123456)\n"
                "  assert(strokes[1].fill == -1 and strokes[1].lineStyle == 'plain')\n"
                "  assert(strokes[1].ref ~= nil and strokes[1].page == 2 and strokes[1].layer == 3)\n"
                "  assert(strokes[2].pressure == nil and strokes[2].page == 1 and strokes[2].layer == 1)\n"
                "  app.changeActionState('stroke-count', #strokes)\n"
                "end\n"
                "function readSelection() app.getStrokes('selection') end\n");
    const auto strokeMutationPlugin = root / "StrokeMutationPlugin";
    writePlugin(strokeMutationPlugin,
                "[about]\ndescription=Create strokes and edit page layers\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Mutate strokes', callback='mutate'})\n"
                "  app.registerUi({menu='Reject malformed strokes', callback='malformed'})\n"
                "  app.registerUi({menu='Export options', callback='exportOptions'})\n"
                "  app.registerUi({menu='Export defaults', callback='exportDefaults'})\n"
                "end\n"
                "function mutate()\n"
                "  local refs = app.addStrokes({strokes={\n"
                "    {x={1,2}, y={3,4}, pressure={0.5,0.75}, tool='pen', width=2.5, color=0x123456, fill=-1, lineStyle='plain'},\n"
                "    {x={5,6}, y={7,8}, tool='highlighter'}\n"
                "  }, allowUndoRedoAction='individual'})\n"
                "  assert(#refs == 2 and refs[1] ~= nil and refs[2] ~= nil)\n"
                "  app.setPageSize(50,60,true)\n"
                "  app.setLayerVisibility(false)\n"
                "  app.setCurrentLayerName('moved for fitting')\n"
                "  app.activateAction('layer-new-above-current')\n"
                "end\n"
                "function exportOptions()\n"
                "  local document = app.getDocumentStructure()\n"
                "  assert(document.xoppFilename == '/tmp/plugin-test.xopp')\n"
                "  app.export({outputFile='/tmp/plugin-test.png', range='2-3; 7', layerRange='1-2', "
                "background='none', progressiveMode=true, backend='cairo', pngDpi=180.0, pngWidth=400.0, "
                "pngHeight=600.0})\n"
                "end\n"
                "function exportDefaults() app.export({outputFile='/tmp/plugin-default.pdf'}) end\n"
                "function malformed() app.addStrokes({strokes={{x={1,2}, y={3}}}}) end\n");
    const auto documentStructurePlugin = root / "DocumentStructurePlugin";
    writePlugin(documentStructurePlugin,
                "[about]\ndescription=Document structure API\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi() app.registerUi({menu='Read structure', callback='read'}) end\n"
                "function read()\n"
                "  local doc = app.getDocumentStructure()\n"
                "  assert(doc.currentPage == 1 and #doc.pages == 1)\n"
                "  assert(doc.pdfBackgroundFilename == '/tmp/background.pdf')\n"
                "  assert(doc.xoppFilename == '/tmp/plugin-test.xopp')\n"
                "  local page = doc.pages[1]\n"
                "  assert(page.pageWidth == 612 and page.pageHeight == 792 and page.isAnnotated)\n"
                "  assert(page.pageTypeFormat == 'graph' and page.pageTypeConfig == 'r1=14.17')\n"
                "  assert(page.backgroundColor == 0x123456 and page.pdfBackgroundPageNo == 3)\n"
                "  assert(page.currentLayer == 2)\n"
                "  assert(page.layers[0].name == 'Paper' and not page.layers[0].isVisible)\n"
                "  assert(page.layers[0].isAnnotated == nil)\n"
                "  assert(page.layers[1].name == 'Ink' and page.layers[1].isVisible)\n"
                "  assert(page.layers[1].isAnnotated)\n"
                "  app.changeActionState('document-page-count', #doc.pages)\n"
                "end\n");
    const auto objectQueryPlugin = root / "ObjectQueryPlugin";
    writePlugin(objectQueryPlugin,
                "[about]\ndescription=Query editable document objects\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read objects', callback='read'})\n"
                "  app.registerUi({menu='Reject invalid references', callback='malformed'})\n"
                "end\n"
                "function read()\n"
                "  local images = app.getImages('selection')\n"
                "  local texts = app.getTexts('all')\n"
                "  local links = app.getLinks('page')\n"
                "  assert(#images == 1 and images[1].x == 11 and images[1].y == 12)\n"
                "  assert(images[1].width == 40 and images[1].height == 30)\n"
                "  assert(images[1].data == 'PNG\\0bytes' and images[1].format == 'png')\n"
                "  assert(images[1].imageWidth == 80 and images[1].imageHeight == 60)\n"
                "  assert(images[1].ref ~= nil and images[1].page == nil and images[1].layer == nil)\n"
                "  assert(#texts == 1 and texts[1].text == 'note')\n"
                "  assert(texts[1].font.name == 'Sans' and texts[1].font.size == 14)\n"
                "  assert(texts[1].color == 0x123456 and texts[1].x == 21 and texts[1].y == 22)\n"
                "  assert(texts[1].wrap == 90 and texts[1].width == 45 and texts[1].height == 18)\n"
                "  assert(texts[1].ref ~= nil and texts[1].page == 2 and texts[1].layer == 3)\n"
                "  assert(#links == 1 and links[1].text == 'site' and links[1].url == 'https://example.test')\n"
                "  assert(links[1].alignment == 1 and links[1].font.name == 'Serif' and links[1].font.size == 12)\n"
                "  assert(links[1].color == 0xabcdef and links[1].x == 31 and links[1].y == 32)\n"
                "  assert(links[1].width == 60 and links[1].height == 16)\n"
                "  assert(links[1].ref ~= nil and links[1].page == nil and links[1].layer == 2)\n"
                "  app.clearSelection()\n"
                "  app.addToSelection({images[1].ref, texts[1].ref, links[1].ref})\n"
                "  app.changeActionState('object-count', #images * 100 + #texts * 10 + #links)\n"
                "end\n"
                "function malformed() app.addToSelection({'not a reference'}) end\n");
    const auto textLinkCreationPlugin = root / "TextLinkCreationPlugin";
    writePlugin(textLinkCreationPlugin,
                "[about]\ndescription=Create editable text and links\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Create text', callback='createText'})\n"
                "  app.registerUi({menu='Create link', callback='createLink'})\n"
                "  app.registerUi({menu='Create individual text', callback='individualText'})\n"
                "  app.registerUi({menu='Create no-undo link', callback='noUndoLink'})\n"
                "  app.registerUi({menu='Reject bad text', callback='badText'})\n"
                "  app.registerUi({menu='Reject bad alignment', callback='badAlignment'})\n"
                "  app.registerUi({menu='Reject bad undo policy', callback='badUndoPolicy'})\n"
                "end\n"
                "function createText()\n"
                "  local refs = app.addTexts({texts={\n"
                "    {text='hello', font={name='Noto Sans', size=9.5}, color=0x1259b9, x=10, y=20, wrap=80},\n"
                "    {text='default font', x=30, y=40}\n"
                "  }, allowUndoRedoAction='grouped'})\n"
                "  assert(#refs == 2 and refs[1] ~= nil and refs[2] ~= nil)\n"
                "end\n"
                "function createLink()\n"
                "  local refs = app.addLinks({links={{text='site', url='https://example.test', "
                "alignment=app.C.Alignment_center, font={name='Serif', size=12}, color=0xabcdef, x=50, y=60}}})\n"
                "  assert(#refs == 1 and refs[1] ~= nil)\n"
                "end\n"
                "function individualText()\n"
                "  local refs = app.addTexts({texts={{text='one undo each', x=5, y=6}}, "
                "allowUndoRedoAction='individual'})\n"
                "  assert(#refs == 1 and refs[1] ~= nil)\n"
                "end\n"
                "function noUndoLink()\n"
                "  local refs = app.addLinks({links={{text='no undo', url='https://example.test', x=7, y=8}}, "
                "allowUndoRedoAction='none'})\n"
                "  assert(#refs == 1 and refs[1] ~= nil)\n"
                "end\n"
                "function badText() app.addTexts({texts={{text='invalid', x=1, y=2, color=0x1000000}}}) end\n"
                "function badAlignment() app.addLinks({links={{text='bad', url='https://example.test', "
                "alignment=4, x=1, y=2}}}) end\n"
                "function badUndoPolicy() app.addTexts({texts={}, allowUndoRedoAction='batched'}) end\n");
    const auto imageCreationPlugin = root / "ImageCreationPlugin";
    writePlugin(imageCreationPlugin,
                "[about]\ndescription=Create editable images\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Create images', callback='create'})\n"
                "  app.registerUi({menu='Reject missing image source', callback='missingSource'})\n"
                "  app.registerUi({menu='Reject two image sources', callback='twoSources'})\n"
                "  app.registerUi({menu='Reject invalid image size', callback='badSize'})\n"
                "  app.registerUi({menu='Reject invalid image scale', callback='badScale'})\n"
                "  app.registerUi({menu='Reject invalid image data', callback='badData'})\n"
                "  app.registerUi({menu='Reject invalid image undo policy', callback='badUndoPolicy'})\n"
                "end\n"
                "function create()\n"
                "  local refs = app.addImages({images={\n"
                "    {data='PNG\\0bytes', x=10, y=20, maxWidth=40, maxHeight=30},\n"
                "    {path='/tmp/plugin-image-missing.png'},\n"
                "    {data='encoded', scale=2, aspectRatio=false}\n"
                "  }, allowUndoRedoAction='individual'})\n"
                "  assert(#refs == 3 and type(refs[1]) == 'userdata' and type(refs[2]) == 'string' and "
                "type(refs[3]) == 'userdata')\n"
                "end\n"
                "function missingSource() app.addImages({images={{x=1, y=2}}}) end\n"
                "function twoSources() app.addImages({images={{path='image.png', data='encoded'}}}) end\n"
                "function badSize() app.addImages({images={{data='encoded', maxWidth=0}}}) end\n"
                "function badScale() app.addImages({images={{data='encoded', scale=0}}}) end\n"
                "function badData() app.addImages({images={{data=42}}}) end\n"
                "function badUndoPolicy() app.addImages({images={}, allowUndoRedoAction='sometimes'}) end\n");
    const auto splineCreationPlugin = root / "SplineCreationPlugin";
    writePlugin(splineCreationPlugin,
                "[about]\ndescription=Create spline strokes\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Create splines', callback='create'})\n"
                "  app.registerUi({menu='Reject missing spline coordinates', callback='missingCoordinates'})\n"
                "  app.registerUi({menu='Reject incomplete spline coordinates', callback='incompleteCoordinates'})\n"
                "  app.registerUi({menu='Reject nonnumeric spline coordinate', callback='badCoordinate'})\n"
                "  app.registerUi({menu='Create spline with default history', callback='defaultHistory'})\n"
                "  app.registerUi({menu='Reject invalid spline undo policy', callback='badUndoPolicy'})\n"
                "end\n"
                "function create()\n"
                "  local refs = app.addSplines({splines={\n"
                "    {coordinates={0,0, 0,100, 100,100, 100,0, 100,0, 130,-50, 170,-50, 200,0},\n"
                "     tool='highlighter', width=3.5, color=0x456789, fill=80, lineStyle='plain'},\n"
                "    {coordinates={210,0, 230,0, 280,0, 300,0}, tool='pen'}\n"
                "  }, allowUndoRedoAction='individual'})\n"
                "  assert(#refs == 1 and type(refs[1]) == 'userdata')\n"
                "end\n"
                "function missingCoordinates() app.addSplines({splines={{tool='pen'}}}) end\n"
                "function incompleteCoordinates() app.addSplines({splines={{coordinates={1,2,3,4,5,6,7}}}}) end\n"
                "function badCoordinate() app.addSplines({splines={{coordinates={1,2,3,4,5,6,'bad',8}}}}) end\n"
                "function defaultHistory() app.addSplines({splines={{coordinates={0,0,0,10,10,10,10,0}}}}) end\n"
                "function badUndoPolicy() app.addSplines({splines={}, allowUndoRedoAction='sometimes'}) end\n");
    const auto zoomApiPlugin = root / "ZoomApiPlugin";
    writePlugin(zoomApiPlugin,
                "[about]\ndescription=Read and change view zoom\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read zoom', callback='read'})\n"
                "  app.registerUi({menu='Set zoom', callback='set'})\n"
                "  app.registerUi({menu='Clamp zoom', callback='clamp'})\n"
                "  app.registerUi({menu='Reject nonnumeric zoom', callback='badType'})\n"
                "  app.registerUi({menu='Reject nonfinite zoom', callback='badNumber'})\n"
                "  app.registerUi({menu='Read display DPI', callback='readDpi'})\n"
                "end\n"
                "function read() assert(app.getZoom() == 1.25) end\n"
                "function set() app.setZoom(2.5); assert(app.getZoom() == 2.5) end\n"
                "function clamp() app.setZoom(15); assert(app.getZoom() == 8) end\n"
                "function badType() app.setZoom('not a number') end\n"
                "function badNumber() app.setZoom(0/0) end\n"
                "function readDpi()\n"
                "  local dpi = app.getDisplayDpi()\n"
                "  assert(math.type(dpi) == 'integer' and dpi == 144)\n"
                "end\n");
    const auto folderApiPlugin = root / "FolderApiPlugin";
    writePlugin(folderApiPlugin,
                "[about]\ndescription=Get persistent plugin folders\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read plugin folders', callback='read'})\n"
                "  app.registerUi({menu='Reject unsupported folder', callback='badType'})\n"
                "end\n"
                "function read()\n"
                "  app.changeActionState('plugin-folder-paths', app.getFolder('config') .. '\\n' .. "
                "      app.getFolder('data') .. '\\n' .. app.getFolder('state'))\n"
                "end\n"
                "function badType() app.getFolder('cache') end\n");
    const auto currentPageApiPlugin = root / "CurrentPageApiPlugin";
    writePlugin(currentPageApiPlugin,
                "[about]\ndescription=Set the current page without scrolling\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Set current page', callback='setPage'})\n"
                "  app.registerUi({menu='Clamp to first page', callback='firstPage'})\n"
                "  app.registerUi({menu='Clamp to last page', callback='lastPage'})\n"
                "  app.registerUi({menu='Clamp negative page', callback='negativePage'})\n"
                "  app.registerUi({menu='Reject fractional page', callback='fractionalPage'})\n"
                "end\n"
                "function setPage() app.setCurrentPage(3) end\n"
                "function firstPage() app.setCurrentPage(0) end\n"
                "function lastPage() app.setCurrentPage(9) end\n"
                "function negativePage() app.setCurrentPage(-2) end\n"
                "function fractionalPage() app.setCurrentPage(1.5) end\n");
    const auto pageLabelApiPlugin = root / "PageLabelApiPlugin";
    const auto zeroPageLabelError = "page nr " + std::to_string(std::numeric_limits<std::size_t>::max()) +
                                    " is out of range";
    writePlugin(pageLabelApiPlugin,
                "[about]\ndescription=Read PDF page labels\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read PDF page label', callback='readLabel'})\n"
                "  app.registerUi({menu='Read missing PDF page label', callback='missingLabel'})\n"
                "  app.registerUi({menu='Read page zero label', callback='zeroLabel'})\n"
                "  app.registerUi({menu='Reject fractional PDF page', callback='fractionalPage'})\n"
                "end\n"
                "function readLabel()\n"
                "  local label = app.getPageLabel(2)\n"
                "  assert(label == 'iv')\n"
                "  app.changeActionState('page-label', label)\n"
                "end\n"
                "function missingLabel()\n"
                "  local label, message = app.getPageLabel(9)\n"
                "  assert(label == nil and message == 'page nr 8 is out of range')\n"
                "  app.changeActionState('page-label', message)\n"
                "end\n"
                "function zeroLabel()\n"
                "  local label, message = app.getPageLabel(0)\n"
                "  assert(label == nil and message == '" + zeroPageLabelError + "')\n"
                "  app.changeActionState('page-label', message)\n"
                "end\n"
                "function fractionalPage() app.getPageLabel(1.5) end\n");
    const auto colorPaletteApiPlugin = root / "ColorPaletteApiPlugin";
    writePlugin(colorPaletteApiPlugin,
                "[about]\ndescription=Read quick colors\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read color palette', callback='readPalette'})\n"
                "end\n"
                "function readPalette()\n"
                "  local palette = app.getColorPalette('ignored extra argument')\n"
                "  assert(#palette == 3)\n"
                "  assert(palette[1].name == 'White' and palette[1].color == 0xffffff)\n"
                "  assert(math.type(palette[1].color) == 'integer')\n"
                "  assert(palette[2].name == 'Dark Blue' and palette[2].color == 0x123456)\n"
                "  assert(palette[3].name == 'Rosewater' and palette[3].color == 0xdc8a78)\n"
                "  app.changeActionState('palette-summary', #palette .. ':' .. palette[2].name .. ':' .. "
                "palette[2].color)\n"
                "end\n");
    const auto currentLayerApiPlugin = root / "CurrentLayerApiPlugin";
    writePlugin(currentLayerApiPlugin,
                "[about]\ndescription=Select the current foreground layer\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Select second layer', callback='selectSecond'})\n"
                "  app.registerUi({menu='Select and reveal through second', callback='revealSecond'})\n"
                "  app.registerUi({menu='Select layer zero', callback='selectZero'})\n"
                "  app.registerUi({menu='Reject missing layer', callback='missingLayer'})\n"
                "  app.registerUi({menu='Reject negative layer', callback='negativeLayer'})\n"
                "  app.registerUi({menu='Reject fractional layer', callback='fractionalLayer'})\n"
                "  app.registerUi({menu='Ignore nonboolean visibility', callback='ignoreVisibility'})\n"
                "end\n"
                "function selectSecond() app.setCurrentLayer(2) end\n"
                "function revealSecond() app.setCurrentLayer(2, true) end\n"
                "function selectZero() app.setCurrentLayer(0, true) end\n"
                "function missingLayer() app.setCurrentLayer(4) end\n"
                "function negativeLayer() app.setCurrentLayer(-1) end\n"
                "function fractionalLayer() app.setCurrentLayer(1.5) end\n"
                "function ignoreVisibility() app.setCurrentLayer(3, 'true') end\n");
    const auto scrollApiPlugin = root / "ScrollApiPlugin";
    writePlugin(scrollApiPlugin,
                "[about]\ndescription=Read and change canvas scrolling\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read scroll position', callback='readPosition'})\n"
                "  app.registerUi({menu='Scroll to last page', callback='lastPage'})\n"
                "  app.registerUi({menu='Scroll to first page', callback='firstPage'})\n"
                "  app.registerUi({menu='Scroll to next page', callback='nextPage'})\n"
                "  app.registerUi({menu='Scroll by position', callback='relativePosition'})\n"
                "  app.registerUi({menu='Scroll to position', callback='absolutePosition'})\n"
                "  app.registerUi({menu='Default relative position', callback='defaultRelative'})\n"
                "  app.registerUi({menu='Reject fractional page', callback='fractionalPage'})\n"
                "end\n"
                "function readPosition()\n"
                "  local position = app.getScrollPos()\n"
                "  assert(position.x == 12 and position.y == 24)\n"
                "  assert(position.width == 640 and position.height == 480)\n"
                "end\n"
                "function lastPage() app.scrollToPage(9) end\n"
                "function firstPage() app.scrollToPage(-8) end\n"
                "function nextPage() app.scrollToPage(1, true) end\n"
                "function relativePosition() app.scrollToPos(20.5, -10.25) end\n"
                "function absolutePosition() app.scrollToPos(200.25, 50.5, false) end\n"
                "function defaultRelative() app.scrollToPos(5, 7, 'false') end\n"
                "function fractionalPage() app.scrollToPage(1.5) end\n");
    const auto fontApiPlugin = root / "FontApiPlugin";
    writePlugin(fontApiPlugin,
                "[about]\ndescription=Read and change text fonts\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read fonts', callback='readFonts'})\n"
                "  app.registerUi({menu='Set font size', callback='setSize'})\n"
                "  app.registerUi({menu='Set font name', callback='setName'})\n"
                "  app.registerUi({menu='Set font description', callback='setDescription'})\n"
                "  app.registerUi({menu='Reject missing font family', callback='badFamily'})\n"
                "  app.registerUi({menu='Reject incomplete font description', callback='badDescription'})\n"
                "  app.registerUi({menu='Reject empty font table', callback='emptyTable'})\n"
                "  app.registerUi({menu='Reject nonfinite font size', callback='nonfiniteSize'})\n"
                "  app.registerUi({menu='Reject wrong font type', callback='wrongType'})\n"
                "end\n"
                "function readFonts()\n"
                "  local fonts = app.getFonts()\n"
                "  local font = app.getFont()\n"
                "  assert(#fonts.families > 0 and fonts.current == 1)\n"
                "  assert(font.name == fonts.families[fonts.current] and font.size == 12.5)\n"
                "end\n"
                "function setSize()\n"
                "  local before = app.getFont()\n"
                "  app.setFont({size=13.25})\n"
                "  local after = app.getFont()\n"
                "  assert(after.name == before.name and after.size == 13.25)\n"
                "end\n"
                "function setName()\n"
                "  local fonts = app.getFonts()\n"
                "  local before = app.getFont()\n"
                "  local name = fonts.families[2] or fonts.families[1]\n"
                "  app.setFont({name=name})\n"
                "  local after = app.getFont()\n"
                "  assert(after.name == name and after.size == before.size)\n"
                "end\n"
                "function setDescription()\n"
                "  local fonts = app.getFonts()\n"
                "  app.setFont(fonts.families[1] .. ' 14')\n"
                "  local after = app.getFont()\n"
                "  assert(after.name == fonts.families[1] and after.size == 14)\n"
                "end\n"
                "function badFamily() app.setFont({name='GTK4 Canvas Missing Family 123'}) end\n"
                "function badDescription() app.setFont('GTK4 Canvas Missing Family 12') end\n"
                "function emptyTable() app.setFont({}) end\n"
                "function nonfiniteSize() app.setFont({size=0/0}) end\n"
                "function wrongType() app.setFont(true) end\n");
    const auto refreshPageApiPlugin = root / "RefreshPageApiPlugin";
    writePlugin(refreshPageApiPlugin,
                "[about]\ndescription=Refresh the current page\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Refresh current page', callback='refresh'})\n"
                "  app.registerUi({menu='Reject refresh without a page', callback='missingPage'})\n"
                "end\n"
                "function refresh() assert(app.refreshPage() == nil) end\n"
                "function missingPage() app.refreshPage() end\n");
    const auto actionStateApiPlugin = root / "ActionStateApiPlugin";
    writePlugin(actionStateApiPlugin,
                "[about]\ndescription=Read supported action state\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read action states', callback='read'})\n"
                "  app.registerUi({menu='Reject unavailable action state', callback='missing'})\n"
                "end\n"
                "function read()\n"
                "  assert(app.getActionState('select-tool') == app.C.Tool_pen)\n"
                "  assert(app.getActionState('zoom') == 1.25)\n"
                "  assert(app.getActionState('grid-snapping') == true)\n"
                "  assert(app.getActionState('position-highlighting') == false)\n"
                "  assert(app.getActionState('tool-color') == 0xff123456)\n"
                "  app.changeActionState('action-state-summary',\n"
                "    tostring(app.getActionState('select-tool')) .. ':' ..\n"
                "    tostring(app.getActionState('zoom')) .. ':' ..\n"
                "    tostring(app.getActionState('tool-color')))\n"
                "end\n"
                "function missing() app.getActionState('missing-action-state') end\n");
    const auto quickScreenshotPlugin = root / "QuickScreenshot";
    writePlugin(quickScreenshotPlugin,
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/QuickScreenshot/plugin.ini"),
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/QuickScreenshot/main.lua"));
    writePlugin(root / "InvalidMain",
                "[about]\ndescription=Invalid\n[default]\nenabled=false\n[plugin]\nmainfile=../outside.lua\n", "");

    prototype::LuaPluginRuntime runtime;
    runtime.discover({root},
                     colorCycle.string() + "," + layerActionsPlugin.string() + "," + quickScreenshotPlugin.string() +
                             "," + textLinkCreationPlugin.string() + "," + imageCreationPlugin.string() + "," +
                             splineCreationPlugin.string() + "," + zoomApiPlugin.string() + "," +
                             folderApiPlugin.string() + "," + currentPageApiPlugin.string() + "," +
                             pageLabelApiPlugin.string() + "," + colorPaletteApiPlugin.string() + "," +
                             currentLayerApiPlugin.string() + "," + scrollApiPlugin.string() + "," +
                             fontApiPlugin.string() + "," + refreshPageApiPlugin.string() + "," +
                             actionStateApiPlugin.string(),
                     {});
    const auto discovered = runtime.plugins();
    require(discovered.size() == 27, "Plugin discovery did not enumerate all plugin directories");
    const auto colorSummary = runtime.findPlugin("ColorCycle");
    require(colorSummary && colorSummary->valid && colorSummary->enabled && !colorSummary->loaded,
            "Explicitly enabled plugin path did not override the disabled default");
    const auto invalidSummary = runtime.findPlugin("InvalidMain");
    require(invalidSummary && !invalidSummary->valid && !invalidSummary->error.empty(),
            "Invalid plugin mainfile was not rejected with a diagnostic");
    const auto invalidApiSummary = runtime.findPlugin("InvalidApi");
    require(invalidApiSummary && invalidApiSummary->valid && invalidApiSummary->enabled,
            "Invalid plugin API fixture was not discovered as an enabled plugin");

    std::uint32_t changedColor = 0;
    bool changedSelection = false;
    std::string changedTool;
    std::int64_t changedMode = -1;
    std::int64_t strokeCount = -1;
    std::int64_t documentPageCount = -1;
    std::int64_t pluginObjectCount = -1;
    std::vector<prototype::PluginStrokeInput> addedStrokes;
    prototype::PluginStrokeUndoPolicy addedStrokePolicy = prototype::PluginStrokeUndoPolicy::Grouped;
    std::vector<prototype::PluginTextInput> addedTexts;
    std::vector<prototype::PluginLinkInput> addedLinks;
    std::vector<prototype::PluginImageInput> addedImages;
    prototype::PluginStrokeUndoPolicy addedTextPolicy = prototype::PluginStrokeUndoPolicy::Individual;
    prototype::PluginStrokeUndoPolicy addedLinkPolicy = prototype::PluginStrokeUndoPolicy::Individual;
    prototype::PluginStrokeUndoPolicy addedImagePolicy = prototype::PluginStrokeUndoPolicy::Grouped;
    double pageWidthChange = 0;
    double pageHeightChange = 0;
    bool pageSizeRelative = false;
    bool exportCalled = false;
    bool quickScreenshotCalled = false;
    std::vector<prototype::DocumentExportOptions> exportRequests;
    bool layerVisible = true;
    std::string renamedLayer;
    std::string imageQueryType;
    std::string textQueryType;
    std::string linkQueryType;
    const std::string pluginImageBytes{"PNG\0bytes", 9};
    std::vector<std::uintptr_t> selectedPluginReferences;
    bool pluginSelectionCleared = false;
    std::string dialogPluginName;
    std::string dialogMessage;
    std::vector<prototype::PluginDialogButton> dialogButtons;
    bool dialogError = true;
    std::function<void(int)> dialogResponse;
    std::string changedBackgroundFormat;
    std::string changedBackgroundConfig;
    std::string pluginFolderPaths;
    std::string pageLabelResult;
    std::string paletteSummary;
    std::string actionStateSummary;
    prototype::PluginFont currentPluginFont{systemFontFamilies.front(), 12.5};
    std::size_t fontChangeCount = 0;
    bool refreshPageAvailable = true;
    std::size_t refreshPageCount = 0;
    std::vector<std::string> activatedActions;
    double zoomValue = 1.25;
    std::size_t currentPageNumber = 2;
    constexpr std::size_t testPageCount = 5;
    std::size_t currentLayerNumber = 1;
    constexpr std::size_t testLayerCount = 3;
    std::vector<bool> testLayerVisibility{true, false, true};
    prototype::PluginScrollPosition scrollPosition{12, 24, 640, 480};
    std::size_t currentScrollPageNumber = 2;
    prototype::PluginRuntimeHost host;
    host.changeToolColor = [&](const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                               std::string&) {
        changedTool = tool.value_or("");
        changedColor = color;
        changedSelection = selection;
        return true;
    };
    host.getActionState = [](std::string_view action, prototype::PluginActionValue& value, std::string& error) {
        if (action == "select-tool") {
            value = std::int64_t{1};
        } else if (action == "zoom") {
            value = 1.25;
        } else if (action == "grid-snapping") {
            value = true;
        } else if (action == "position-highlighting") {
            value = false;
        } else if (action == "tool-color") {
            value = std::int64_t{0xff123456U};
        } else {
            error = "Action state is unavailable in this GTK 4 host: " + std::string(action);
            return false;
        }
        return true;
    };
    host.changeActionState = [&](std::string_view action, const prototype::PluginActionValue& value, std::string&) {
        if (action == "action-state-summary") {
            if (!std::holds_alternative<std::string>(value)) {
                return false;
            }
            actionStateSummary = std::get<std::string>(value);
            return true;
        }
        if (action == "palette-summary") {
            if (!std::holds_alternative<std::string>(value)) {
                return false;
            }
            paletteSummary = std::get<std::string>(value);
            return true;
        }
        if (action == "page-label") {
            if (!std::holds_alternative<std::string>(value)) {
                return false;
            }
            pageLabelResult = std::get<std::string>(value);
            return true;
        }
        if (action == "plugin-folder-paths") {
            if (!std::holds_alternative<std::string>(value)) {
                return false;
            }
            pluginFolderPaths = std::get<std::string>(value);
            return true;
        }
        if ((action != "test-mode" && action != "stroke-count" && action != "document-page-count" &&
             action != "object-count") ||
            !std::holds_alternative<std::int64_t>(value)) {
            return false;
        }
        if (action == "test-mode") {
            changedMode = std::get<std::int64_t>(value);
        } else if (action == "document-page-count") {
            documentPageCount = std::get<std::int64_t>(value);
        } else if (action == "object-count") {
            pluginObjectCount = std::get<std::int64_t>(value);
        } else {
            strokeCount = std::get<std::int64_t>(value);
        }
        return true;
    };
    host.getZoom = [&] { return zoomValue; };
    host.setZoom = [&](double zoom, std::string&) {
        zoomValue = std::clamp(zoom, 0.1, 8.0);
        return true;
    };
    host.getDisplayDpi = [] { return 144; };
    const std::vector<std::string> testPageLabels{"i", "iv", "7"};
    host.getPageLabel = [&](std::size_t pageNumber, std::string& label, std::string& error) {
        if (pageNumber >= testPageLabels.size()) {
            error = "page nr " + std::to_string(pageNumber) + " is out of range";
            return false;
        }
        label = testPageLabels[pageNumber];
        return true;
    };
    const std::vector<prototype::PluginPaletteColor> testPalette{
            {"White", 0x12ffffffU},
            {"Dark Blue", 0x123456U},
            {"Rosewater", 0xdc8a78U},
    };
    host.getColorPalette = [&] { return testPalette; };
    host.refreshPage = [&](std::string& error) {
        if (!refreshPageAvailable) {
            error = "Called applib_refreshPage, but there is no current page.";
            return false;
        }
        ++refreshPageCount;
        return true;
    };
    host.getFont = [&] { return currentPluginFont; };
    host.setFont = [&](const prototype::PluginFont& font, std::string&) {
        currentPluginFont = font;
        ++fontChangeCount;
        return true;
    };
    host.getScrollPosition = [&] { return scrollPosition; };
    host.scrollToPage = [&](int pageNumber, bool relative, std::string&) {
        const auto requestedPage = relative ? static_cast<std::int64_t>(currentScrollPageNumber - 1) + pageNumber :
                                             static_cast<std::int64_t>(pageNumber) - 1;
        const auto targetPage = std::clamp(requestedPage, std::int64_t{0},
                                           static_cast<std::int64_t>(testPageCount - 1));
        currentScrollPageNumber = static_cast<std::size_t>(targetPage + 1);
        return true;
    };
    host.scrollToPosition = [&](double x, double y, bool relative, std::string&) {
        if (relative) {
            scrollPosition.x += x;
            scrollPosition.y += y;
        } else {
            scrollPosition.x = x;
            scrollPosition.y = y;
        }
        return true;
    };
    host.setCurrentPage = [&](std::size_t pageNumber, std::string&) {
        currentPageNumber = std::clamp(pageNumber, std::size_t{1}, testPageCount);
        return true;
    };
    host.setCurrentLayer = [&](std::size_t layerNumber, bool updateVisibility, std::string& error) {
        if (layerNumber > testLayerCount) {
            error = "No layer with layer ID " + std::to_string(static_cast<std::int64_t>(layerNumber));
            return false;
        }
        currentLayerNumber = std::max<std::size_t>(1, layerNumber);
        if (updateVisibility) {
            for (std::size_t index = 0; index < testLayerVisibility.size(); ++index) {
                testLayerVisibility[index] = index + 1 <= layerNumber;
            }
        }
        return true;
    };
    host.getStrokes = [](std::string_view type, std::vector<prototype::PluginStroke>& strokes,
                         std::string& error) {
        if (type == "selection") {
            error = "There is no selection";
            return false;
        }
        if (type != "all") {
            error = "Unexpected stroke query type";
            return false;
        }
        strokes = {
                {{10, 12.5}, {22, 24.5}, {0.25, 0.75}, "pen", 2.25, 0x123456, -1, "plain", 0x42, 2, 3},
                {{31, 40}, {42, 51}, {}, "highlighter", 8.5, 0xffcc00, -1, "plain", 0x43, 1, 1},
        };
        return true;
    };
    host.addStrokes = [&](const std::vector<prototype::PluginStrokeInput>& strokes,
                          prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                          std::string&) {
        addedStrokes = strokes;
        addedStrokePolicy = policy;
        references.clear();
        for (std::size_t index = 0; index < strokes.size(); ++index) {
            references.push_back(0x51 + index);
        }
        return true;
    };
    host.addTexts = [&](const std::vector<prototype::PluginTextInput>& texts,
                        prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                        std::string&) {
        addedTexts = texts;
        addedTextPolicy = policy;
        references.clear();
        for (std::size_t index = 0; index < texts.size(); ++index) {
            references.push_back(0x71 + index);
        }
        return true;
    };
    host.addLinks = [&](const std::vector<prototype::PluginLinkInput>& links,
                        prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                        std::string&) {
        addedLinks = links;
        addedLinkPolicy = policy;
        references.clear();
        for (std::size_t index = 0; index < links.size(); ++index) {
            references.push_back(0x73 + index);
        }
        return true;
    };
    host.addImages = [&](const std::vector<prototype::PluginImageInput>& images,
                         prototype::PluginStrokeUndoPolicy policy,
                         std::vector<prototype::PluginImageResult>& results, std::string&) {
        addedImages = images;
        addedImagePolicy = policy;
        results.clear();
        for (std::size_t index = 0; index < images.size(); ++index) {
            if (images[index].path) {
                results.push_back({std::nullopt, "Error: missing fixture image"});
            } else {
                results.push_back({0x81 + index, {}});
            }
        }
        return true;
    };
    host.getImages = [&](std::string_view type, std::vector<prototype::PluginImage>& images, std::string&) {
        imageQueryType = type;
        images = {{11, 12, 40, 30, pluginImageBytes, "png", 80, 60, 0x61, std::nullopt, std::nullopt}};
        return true;
    };
    host.getTexts = [&](std::string_view type, std::vector<prototype::PluginText>& texts, std::string&) {
        textQueryType = type;
        texts = {{"note", "Sans", 14, 0x123456, 21, 22, 90, 45, 18, 0x62, 2, 3}};
        return true;
    };
    host.getLinks = [&](std::string_view type, std::vector<prototype::PluginLink>& links, std::string&) {
        linkQueryType = type;
        links = {{"site", "https://example.test", 1, "Serif", 12, 0xabcdef, 31, 32, 60, 16, 0x63,
                  std::nullopt, 2}};
        return true;
    };
    host.addToSelection = [&](const std::vector<std::uintptr_t>& references, std::string&) {
        selectedPluginReferences = references;
        return true;
    };
    host.clearSelection = [&] { pluginSelectionCleared = true; };
    host.setPageSize = [&](double width, double height, bool relative, std::string&) {
        pageWidthChange = width;
        pageHeightChange = height;
        pageSizeRelative = relative;
        return true;
    };
    host.setLayerVisibility = [&](bool visible, std::string&) {
        layerVisible = visible;
        return true;
    };
    host.setCurrentLayerName = [&](std::string_view name, std::string&) {
        renamedLayer = name;
        return true;
    };
    host.exportDocument = [&](const prototype::DocumentExportOptions& options, std::string&) {
        exportCalled = true;
        exportRequests.push_back(options);
        return true;
    };
    host.quickScreenshot = [&](std::string_view plugin, std::string&) {
        quickScreenshotCalled = plugin == "QuickScreenshot";
        return quickScreenshotCalled;
    };
    host.documentFilename = [] { return std::string("/tmp/plugin-test.xopp"); };
    host.documentStructure = [] {
        prototype::PluginDocumentStructure structure;
        structure.pages = {{612, 792, true, "graph", "r1=14.17", 0x123456, 3,
                            {{false, "Paper", std::nullopt}, {true, "Ink", true}}, 2}};
        structure.currentPage = 1;
        structure.pdfBackgroundFilename = "/tmp/background.pdf";
        structure.xoppFilename = "/tmp/plugin-test.xopp";
        return structure;
    };
    host.changeCurrentPageBackground = [&](std::string_view format, std::string_view config, std::string&) {
        changedBackgroundFormat = format;
        changedBackgroundConfig = config;
        return true;
    };
    host.activateAction = [&](std::string_view action, std::string& error) {
        if (action != "beamer-next" && action != "beamer-previous" && action != "beamer-clean" &&
            action != "layer-actions-clone-next" && action != "layer-actions-hide" &&
            action != "layer-actions-add-top" && action != "layer-new-above-current") {
            error = "Actions are unavailable in this GTK 4 host";
            return false;
        }
        activatedActions.emplace_back(action);
        return true;
    };
    host.openDialog = [&](std::string_view pluginName, std::string_view message,
                          const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                          std::function<void(int)> response, std::string&) {
        dialogPluginName = pluginName;
        dialogMessage = message;
        dialogButtons = buttons;
        dialogError = isError;
        dialogResponse = std::move(response);
        return true;
    };
    runtime.loadEnabled(std::move(host));

    const auto loadedColorSummary = runtime.findPlugin("ColorCycle");
    require(loadedColorSummary && loadedColorSummary->loaded && loadedColorSummary->menuItems == 1,
            "Enabled Lua plugin did not run initUi and register its menu callback");
    const auto loadedDefaultSummary = runtime.findPlugin("DefaultPlugin");
    require(loadedDefaultSummary && loadedDefaultSummary->loaded, "Default-enabled plugin did not load");
    const auto loadedDialogPlugin = runtime.findPlugin("DialogPlugin");
    require(loadedDialogPlugin && loadedDialogPlugin->loaded && loadedDialogPlugin->menuItems == 2,
            "Plugin using app.openDialog did not load and register its menu callback");
    const auto loadedStrokeApiPlugin = runtime.findPlugin("StrokeApiPlugin");
    require(loadedStrokeApiPlugin && loadedStrokeApiPlugin->loaded && loadedStrokeApiPlugin->menuItems == 2,
            "Plugin using app.getStrokes did not load and register its menu callbacks");
    const auto loadedStrokeMutationPlugin = runtime.findPlugin("StrokeMutationPlugin");
    require(loadedStrokeMutationPlugin && loadedStrokeMutationPlugin->loaded &&
                    loadedStrokeMutationPlugin->menuItems == 4,
            "Plugin using stroke creation and layer/page APIs did not load its callbacks");
    const auto loadedDocumentStructurePlugin = runtime.findPlugin("DocumentStructurePlugin");
    require(loadedDocumentStructurePlugin && loadedDocumentStructurePlugin->loaded &&
                    loadedDocumentStructurePlugin->menuItems == 1,
            "Plugin using the complete document-structure API did not load");
    const auto loadedObjectQueryPlugin = runtime.findPlugin("ObjectQueryPlugin");
    require(loadedObjectQueryPlugin && loadedObjectQueryPlugin->loaded && loadedObjectQueryPlugin->menuItems == 2,
            "Plugin using editable object queries and selection references did not load");
    const auto loadedTextLinkCreationPlugin = runtime.findPlugin("TextLinkCreationPlugin");
    require(loadedTextLinkCreationPlugin && loadedTextLinkCreationPlugin->loaded &&
                    loadedTextLinkCreationPlugin->menuItems == 7,
            "Plugin using text and link creation did not load its callbacks");
    const auto loadedImageCreationPlugin = runtime.findPlugin("ImageCreationPlugin");
    require(loadedImageCreationPlugin && loadedImageCreationPlugin->loaded &&
                    loadedImageCreationPlugin->menuItems == 7,
            "Plugin using image creation did not load its callbacks");
    const auto loadedSplineCreationPlugin = runtime.findPlugin("SplineCreationPlugin");
    require(loadedSplineCreationPlugin && loadedSplineCreationPlugin->loaded &&
                    loadedSplineCreationPlugin->menuItems == 6,
            "Plugin using spline creation did not load its callbacks");
    const auto loadedZoomApiPlugin = runtime.findPlugin("ZoomApiPlugin");
    require(loadedZoomApiPlugin && loadedZoomApiPlugin->loaded && loadedZoomApiPlugin->menuItems == 6,
            "Plugin using zoom and display DPI APIs did not load its callbacks");
    const auto loadedFolderApiPlugin = runtime.findPlugin("FolderApiPlugin");
    require(loadedFolderApiPlugin && loadedFolderApiPlugin->loaded && loadedFolderApiPlugin->menuItems == 2,
            "Plugin using persistent plugin folders did not load its callbacks");
    const auto loadedCurrentPageApiPlugin = runtime.findPlugin("CurrentPageApiPlugin");
    require(loadedCurrentPageApiPlugin && loadedCurrentPageApiPlugin->loaded &&
                    loadedCurrentPageApiPlugin->menuItems == 5,
            "Plugin using current-page selection did not load its callbacks");
    const auto loadedPageLabelApiPlugin = runtime.findPlugin("PageLabelApiPlugin");
    require(loadedPageLabelApiPlugin && loadedPageLabelApiPlugin->loaded &&
                    loadedPageLabelApiPlugin->menuItems == 4,
            "Plugin using PDF page labels did not load its callbacks");
    const auto loadedColorPaletteApiPlugin = runtime.findPlugin("ColorPaletteApiPlugin");
    require(loadedColorPaletteApiPlugin && loadedColorPaletteApiPlugin->loaded &&
                    loadedColorPaletteApiPlugin->menuItems == 1,
            "Plugin using the color palette API did not load its callback");
    const auto loadedCurrentLayerApiPlugin = runtime.findPlugin("CurrentLayerApiPlugin");
    require(loadedCurrentLayerApiPlugin && loadedCurrentLayerApiPlugin->loaded &&
                    loadedCurrentLayerApiPlugin->menuItems == 7,
            "Plugin using current-layer selection did not load its callbacks");
    const auto loadedScrollApiPlugin = runtime.findPlugin("ScrollApiPlugin");
    require(loadedScrollApiPlugin && loadedScrollApiPlugin->loaded && loadedScrollApiPlugin->menuItems == 8,
            "Plugin using canvas scrolling did not load its callbacks");
    const auto loadedQuickScreenshotPlugin = runtime.findPlugin("QuickScreenshot");
    require(loadedQuickScreenshotPlugin && loadedQuickScreenshotPlugin->loaded &&
                    loadedQuickScreenshotPlugin->menuItems == 1,
            "Bundled QuickScreenshot plugin did not register its menu callback through the GTK4 host");
    require(!runtime.findPlugin("InvalidMain")->loaded, "Invalid plugin mainfile unexpectedly loaded");
    const auto loadedInvalidApi = runtime.findPlugin("InvalidApi");
    require(loadedInvalidApi && !loadedInvalidApi->loaded &&
                    loadedInvalidApi->error.find("must be an integer") != std::string::npos,
            "Invalid Lua plugin API values did not produce a runtime diagnostic");
    require(!runtime.invokeMenu("UnavailableAction", 0),
            "A failed host action should be reported as a Lua callback error");
    const auto unavailableSummary = runtime.findPlugin("UnavailableAction");
    require(unavailableSummary && unavailableSummary->error.find("Actions are unavailable") != std::string::npos,
            "Unavailable host action did not retain a plugin diagnostic");

    require(runtime.invokeMenu("ColorCycle", 0), "Registered plugin menu callback did not execute");
    require(changedColor == 0x123456 && changedTool == "pen" && changedSelection && changedMode == 7,
            "Lua app API arguments did not reach the GTK4 host callback");
    require(runtime.invokeMenu("ZoomApiPlugin", 0) && zoomValue == 1.25,
            "app.getZoom did not return the current view zoom");
    require(runtime.invokeMenu("ZoomApiPlugin", 1) && zoomValue == 2.5,
            "app.setZoom did not update the current view zoom");
    require(runtime.invokeMenu("ZoomApiPlugin", 2) && zoomValue == 8.0,
            "app.setZoom did not clamp the zoom to the GTK4 view range");
    require(!runtime.invokeMenu("ZoomApiPlugin", 3) &&
                    runtime.findPlugin("ZoomApiPlugin")->error.find("expects a number") != std::string::npos,
            "app.setZoom accepted a nonnumeric value");
    require(!runtime.invokeMenu("ZoomApiPlugin", 4) &&
                    runtime.findPlugin("ZoomApiPlugin")->error.find("finite number") != std::string::npos,
            "app.setZoom accepted a nonfinite value");
    require(runtime.invokeMenu("ZoomApiPlugin", 5),
            "app.getDisplayDpi did not return the host display DPI as a Lua integer");
    require(runtime.invokeMenu("FolderApiPlugin", 0) &&
                    pluginFolderPaths == (xdgConfigRoot / "xournalpp" / "plugin-settings" / "FolderApiPlugin")
                                                  .string() + "\n" +
                                                  (xdgDataRoot / "xournalpp" / "plugin-data" / "FolderApiPlugin")
                                                          .string() + "\n" +
                                                  (expectedStateRoot / "xournalpp" / "plugin-state" / "FolderApiPlugin")
                                                          .string() &&
                    std::filesystem::is_directory(xdgConfigRoot / "xournalpp" / "plugin-settings" /
                                                  "FolderApiPlugin") &&
                    std::filesystem::is_directory(xdgDataRoot / "xournalpp" / "plugin-data" / "FolderApiPlugin") &&
                    std::filesystem::is_directory(expectedStateRoot / "xournalpp" / "plugin-state" /
                                                  "FolderApiPlugin"),
            "app.getFolder did not create and return the plugin's config, data, and state directories");
    require(!runtime.invokeMenu("FolderApiPlugin", 1) &&
                    runtime.findPlugin("FolderApiPlugin")->error.find("Unsupported folder type 'cache'") !=
                            std::string::npos,
            "app.getFolder accepted an unsupported folder type");
    require(runtime.invokeMenu("CurrentPageApiPlugin", 0) && currentPageNumber == 3,
            "app.setCurrentPage did not forward the one-based page number");
    require(runtime.invokeMenu("CurrentPageApiPlugin", 1) && currentPageNumber == 1,
            "app.setCurrentPage did not clamp page zero to the first page");
    require(runtime.invokeMenu("CurrentPageApiPlugin", 2) && currentPageNumber == testPageCount,
            "app.setCurrentPage did not clamp an oversized page to the last page");
    require(runtime.invokeMenu("CurrentPageApiPlugin", 3) && currentPageNumber == testPageCount,
            "app.setCurrentPage did not preserve upstream behavior for negative page numbers");
    require(!runtime.invokeMenu("CurrentPageApiPlugin", 4) &&
                    runtime.findPlugin("CurrentPageApiPlugin")->error.find("integer") != std::string::npos,
            "app.setCurrentPage accepted a fractional page number");
    require(runtime.invokeMenu("PageLabelApiPlugin", 0) && pageLabelResult == "iv",
            "app.getPageLabel did not return the requested PDF page label");
    require(runtime.invokeMenu("PageLabelApiPlugin", 1) &&
                    pageLabelResult == "page nr 8 is out of range",
            "app.getPageLabel did not return nil and a compatible message for an out-of-range page");
    require(runtime.invokeMenu("PageLabelApiPlugin", 2) &&
                    pageLabelResult == zeroPageLabelError,
            "app.getPageLabel changed the master's zero-page out-of-range behavior");
    require(!runtime.invokeMenu("PageLabelApiPlugin", 3) &&
                    runtime.findPlugin("PageLabelApiPlugin")->error.find("number has no integer representation") !=
                            std::string::npos,
            "app.getPageLabel accepted a fractional page number");
    require(runtime.invokeMenu("ColorPaletteApiPlugin", 0) && paletteSummary == "3:Dark Blue:1193046",
            "app.getColorPalette did not preserve swatch order, names, or 24-bit integer colors");
    require(runtime.invokeMenu("CurrentLayerApiPlugin", 0) && currentLayerNumber == 2 &&
                    testLayerVisibility == std::vector<bool>({true, false, true}),
            "app.setCurrentLayer changed visibility when its option was omitted");
    require(runtime.invokeMenu("CurrentLayerApiPlugin", 1) && currentLayerNumber == 2 &&
                    testLayerVisibility == std::vector<bool>({true, true, false}),
            "app.setCurrentLayer did not reveal layers through the selected foreground layer");
    require(runtime.invokeMenu("CurrentLayerApiPlugin", 2) && currentLayerNumber == 1 &&
                    testLayerVisibility == std::vector<bool>({false, false, false}),
            "app.setCurrentLayer did not preserve the legacy layer-zero selection and visibility behavior");
    require(!runtime.invokeMenu("CurrentLayerApiPlugin", 3) &&
                    runtime.findPlugin("CurrentLayerApiPlugin")->error.find("No layer with layer ID 4") !=
                            std::string::npos,
            "app.setCurrentLayer accepted a missing layer");
    require(!runtime.invokeMenu("CurrentLayerApiPlugin", 4) &&
                    runtime.findPlugin("CurrentLayerApiPlugin")->error.find("No layer with layer ID -1") !=
                            std::string::npos,
            "app.setCurrentLayer accepted a negative layer number");
    require(!runtime.invokeMenu("CurrentLayerApiPlugin", 5) &&
                    runtime.findPlugin("CurrentLayerApiPlugin")->error.find("number has no integer representation") !=
                            std::string::npos,
            "app.setCurrentLayer accepted a fractional layer number");
    require(runtime.invokeMenu("CurrentLayerApiPlugin", 6) && currentLayerNumber == 3 &&
                    testLayerVisibility == std::vector<bool>({false, false, false}),
            "app.setCurrentLayer did not ignore a nonboolean visibility option");
    require(runtime.invokeMenu("ScrollApiPlugin", 0), "app.getScrollPos did not return the scroll viewport fields");
    require(runtime.invokeMenu("ScrollApiPlugin", 1) && currentScrollPageNumber == testPageCount,
            "app.scrollToPage did not clamp an absolute page number to the last page");
    require(runtime.invokeMenu("ScrollApiPlugin", 2) && currentScrollPageNumber == 1,
            "app.scrollToPage did not clamp an absolute page number to the first page");
    require(runtime.invokeMenu("ScrollApiPlugin", 3) && currentScrollPageNumber == 2,
            "app.scrollToPage did not apply a relative page offset");
    require(runtime.invokeMenu("ScrollApiPlugin", 4) && scrollPosition.x == 32.5 && scrollPosition.y == 13.75,
            "app.scrollToPos did not use relative scrolling by default");
    require(runtime.invokeMenu("ScrollApiPlugin", 5) && scrollPosition.x == 200.25 && scrollPosition.y == 50.5,
            "app.scrollToPos did not support absolute coordinates");
    require(runtime.invokeMenu("ScrollApiPlugin", 6) && scrollPosition.x == 205.25 && scrollPosition.y == 57.5,
            "app.scrollToPos did not ignore a nonboolean mode argument and retain its relative default");
    require(!runtime.invokeMenu("ScrollApiPlugin", 7) &&
                    runtime.findPlugin("ScrollApiPlugin")->error.find("number has no integer representation") !=
                            std::string::npos,
            "app.scrollToPage accepted a fractional page number");
    require(runtime.invokeMenu("FontApiPlugin", 0),
            "app.getFonts/app.getFont did not return the available font families and current font");
    require(runtime.invokeMenu("FontApiPlugin", 1) && currentPluginFont.name == systemFontFamilies.front() &&
                    currentPluginFont.size == 13.25,
            "app.setFont did not apply a partial size change while preserving the font name");
    require(runtime.invokeMenu("FontApiPlugin", 2) && currentPluginFont.size == 13.25,
            "app.setFont did not apply a partial name change while preserving the font size");
    require(runtime.invokeMenu("FontApiPlugin", 3) && currentPluginFont.name == systemFontFamilies.front() &&
                    currentPluginFont.size == 14 && fontChangeCount == 3,
            "app.setFont did not parse and apply a Pango font description");
    require(!runtime.invokeMenu("FontApiPlugin", 4) &&
                    runtime.findPlugin("FontApiPlugin")->error.find("not available on this system") !=
                            std::string::npos,
            "app.setFont accepted a family that is not installed");
    require(!runtime.invokeMenu("FontApiPlugin", 5) &&
                    runtime.findPlugin("FontApiPlugin")->error.find("Font description") != std::string::npos,
            "app.setFont accepted a description with a missing font family");
    require(!runtime.invokeMenu("FontApiPlugin", 6) &&
                    runtime.findPlugin("FontApiPlugin")->error.find("at least 'name' or 'size'") != std::string::npos,
            "app.setFont accepted an empty font table");
    require(!runtime.invokeMenu("FontApiPlugin", 7) &&
                    runtime.findPlugin("FontApiPlugin")->error.find("finite number") != std::string::npos,
            "app.setFont accepted a nonfinite size");
    require(!runtime.invokeMenu("FontApiPlugin", 8) &&
                    runtime.findPlugin("FontApiPlugin")->error.find("either a string or a table") != std::string::npos,
            "app.setFont accepted a value that is neither a string nor a table");
    require(runtime.invokeMenu("RefreshPageApiPlugin", 0) && refreshPageCount == 1,
            "app.refreshPage did not refresh the host or incorrectly returned a value");
    refreshPageAvailable = false;
    require(!runtime.invokeMenu("RefreshPageApiPlugin", 1) &&
                    runtime.findPlugin("RefreshPageApiPlugin")->error.find(
                            "Called applib_refreshPage, but there is no current page.") != std::string::npos,
            "app.refreshPage did not propagate the missing-page error");
    refreshPageAvailable = true;
    require(runtime.invokeMenu("ActionStateApiPlugin", 0) && actionStateSummary == "1:1.25:4279383126",
            "app.getActionState did not preserve boolean, integer, and numeric action-state values");
    require(!runtime.invokeMenu("ActionStateApiPlugin", 1) &&
                    runtime.findPlugin("ActionStateApiPlugin")->error.find("unavailable") != std::string::npos,
            "app.getActionState did not propagate an unavailable action error");
    require(runtime.invokeMenu("BackgroundPlugin", 0) && changedBackgroundFormat == "graph" &&
                    changedBackgroundConfig == "r1=14.17",
            "Page background type and configuration did not reach the GTK4 host callback");
    require(runtime.invokeMenu("DocumentStructurePlugin", 0) && documentPageCount == 1,
            "app.getDocumentStructure did not expose the upstream page, background, and layer fields");
    require(runtime.invokeMenu("StrokeApiPlugin", 0) && strokeCount == 2,
            "app.getStrokes did not preserve point arrays, pressure, style fields, references, and page/layer data");
    require(runtime.invokeMenu("ObjectQueryPlugin", 0) && pluginObjectCount == 111 &&
                    imageQueryType == "selection" && textQueryType == "all" && linkQueryType == "page" &&
                    pluginSelectionCleared && selectedPluginReferences ==
                            std::vector<std::uintptr_t>({0x61, 0x62, 0x63}),
            "app.getImages/getTexts/getLinks or reference-based selection lost fields or references");
    require(!runtime.invokeMenu("ObjectQueryPlugin", 1) &&
                    runtime.findPlugin("ObjectQueryPlugin")->error.find("array of element references") !=
                            std::string::npos,
            "app.addToSelection accepted a non-reference value");
    require(runtime.invokeMenu("TextLinkCreationPlugin", 0) && addedTexts.size() == 2 &&
                    addedTextPolicy == prototype::PluginStrokeUndoPolicy::Grouped &&
                    addedTexts[0].text == "hello" &&
                    addedTexts[0].fontName == std::optional<std::string>("Noto Sans") &&
                    addedTexts[0].fontSize == std::optional<double>(9.5) &&
                    addedTexts[0].color == std::optional<std::uint32_t>(0x1259b9) &&
                    addedTexts[0].x == 10 && addedTexts[0].y == 20 && addedTexts[0].wrap == 80 &&
                    addedTexts[1].text == "default font" && !addedTexts[1].fontName &&
                    !addedTexts[1].fontSize && !addedTexts[1].color && !addedTexts[1].wrap,
            "app.addTexts lost its optional fields, defaults, undo policy, or returned references");
    require(runtime.invokeMenu("TextLinkCreationPlugin", 1) && addedLinks.size() == 1 &&
                    addedLinkPolicy == prototype::PluginStrokeUndoPolicy::Grouped &&
                    addedLinks[0].text == "site" && addedLinks[0].url == "https://example.test" &&
                    addedLinks[0].alignment == std::optional<int>(1) &&
                    addedLinks[0].fontName == std::optional<std::string>("Serif") &&
                    addedLinks[0].fontSize == std::optional<double>(12) &&
                    addedLinks[0].color == std::optional<std::uint32_t>(0xabcdef) &&
                    addedLinks[0].x == 50 && addedLinks[0].y == 60,
            "app.addLinks lost URL, alignment, style, coordinates, defaults, or returned references");
    require(runtime.invokeMenu("TextLinkCreationPlugin", 2) && addedTexts.size() == 1 &&
                    addedTextPolicy == prototype::PluginStrokeUndoPolicy::Individual &&
                    !addedTexts[0].fontName && !addedTexts[0].fontSize && !addedTexts[0].color &&
                    addedTexts[0].x == 5 && addedTexts[0].y == 6,
            "app.addTexts did not preserve individual undo policy or optional defaults");
    require(runtime.invokeMenu("TextLinkCreationPlugin", 3) && addedLinks.size() == 1 &&
                    addedLinkPolicy == prototype::PluginStrokeUndoPolicy::None &&
                    !addedLinks[0].alignment && !addedLinks[0].fontName && !addedLinks[0].fontSize &&
                    !addedLinks[0].color && addedLinks[0].x == 7 && addedLinks[0].y == 8,
            "app.addLinks did not preserve no-undo policy or default alignment/style");
    require(!runtime.invokeMenu("TextLinkCreationPlugin", 4) &&
                    runtime.findPlugin("TextLinkCreationPlugin")->error.find("0x000000..0xFFFFFF") !=
                            std::string::npos,
            "app.addTexts accepted an out-of-range color");
    require(!runtime.invokeMenu("TextLinkCreationPlugin", 5) &&
                    runtime.findPlugin("TextLinkCreationPlugin")->error.find("alignment must be 0, 1, or 2") !=
                            std::string::npos,
            "app.addLinks accepted an invalid alignment");
    require(!runtime.invokeMenu("TextLinkCreationPlugin", 6) &&
                    runtime.findPlugin("TextLinkCreationPlugin")->error.find("Unrecognized undo option") !=
                            std::string::npos,
            "app.addTexts accepted an unknown undo policy");
    require(runtime.invokeMenu("ImageCreationPlugin", 0) && addedImages.size() == 3 &&
                    addedImagePolicy == prototype::PluginStrokeUndoPolicy::Individual &&
                    addedImages[0].data == std::optional<std::string>(std::string("PNG\0bytes", 9)) &&
                    addedImages[0].x == 10 && addedImages[0].y == 20 && addedImages[0].maxWidth == 40 &&
                    addedImages[0].maxHeight == 30 && addedImages[0].scale == 1 && addedImages[0].aspectRatio &&
                    addedImages[1].path == std::optional<std::string>("/tmp/plugin-image-missing.png") &&
                    !addedImages[1].data && addedImages[1].x == 0 && addedImages[1].y == 0 &&
                    addedImages[1].maxWidth == -1 && addedImages[1].maxHeight == -1 &&
                    addedImages[1].scale == 1 && addedImages[1].aspectRatio &&
                    addedImages[2].data == std::optional<std::string>("encoded") &&
                    addedImages[2].scale == 2 && !addedImages[2].aspectRatio,
            "app.addImages lost binary data, source selection, sizing options, defaults, or undo policy");
    require(!runtime.invokeMenu("ImageCreationPlugin", 1) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("No 'path' parameter") !=
                            std::string::npos,
            "app.addImages accepted an entry without a path or data source");
    require(!runtime.invokeMenu("ImageCreationPlugin", 2) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("Only one of 'path' or image 'data'") !=
                            std::string::npos,
            "app.addImages accepted both a path and image data");
    require(!runtime.invokeMenu("ImageCreationPlugin", 3) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("positive integers or -1") !=
                            std::string::npos,
            "app.addImages accepted an invalid maximum dimension");
    require(!runtime.invokeMenu("ImageCreationPlugin", 4) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("scale must be positive") !=
                            std::string::npos,
            "app.addImages accepted a nonpositive scale");
    require(!runtime.invokeMenu("ImageCreationPlugin", 5) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("Field 'data' must be a string") !=
                            std::string::npos,
            "app.addImages accepted non-binary data");
    require(!runtime.invokeMenu("ImageCreationPlugin", 6) &&
                    runtime.findPlugin("ImageCreationPlugin")->error.find("Unrecognized undo option") !=
                            std::string::npos,
            "app.addImages accepted an unknown undo policy");
    require(runtime.invokeMenu("SplineCreationPlugin", 0) && addedStrokes.size() == 1 &&
                    addedStrokePolicy == prototype::PluginStrokeUndoPolicy::Individual &&
                    addedStrokes[0].x.size() > 2 && addedStrokes[0].x.size() == addedStrokes[0].y.size() &&
                    addedStrokes[0].x.front() == 0 && addedStrokes[0].y.front() == 0 &&
                    addedStrokes[0].tool == std::optional<std::string>("highlighter") &&
                    addedStrokes[0].width == std::optional<double>(3.5) &&
                    addedStrokes[0].color == std::optional<std::uint32_t>(0x456789) &&
                    addedStrokes[0].fill == std::optional<int>(80) &&
                    addedStrokes[0].lineStyle == std::optional<std::string>("plain"),
            "app.addSplines did not flatten cubic segments, skip short strokes, or preserve options");
    const bool hasSplineJoin = [&] {
        for (std::size_t index = 0; index < addedStrokes[0].x.size(); ++index) {
            if (std::abs(addedStrokes[0].x[index] - 100) < 1e-9 &&
                std::abs(addedStrokes[0].y[index]) < 1e-9) {
                return true;
            }
        }
        return false;
    }();
    require(hasSplineJoin && std::abs(addedStrokes[0].x.back() - 200) > 1e-6,
            "app.addSplines lost the segment join or changed the master's omitted final endpoint");
    require(!runtime.invokeMenu("SplineCreationPlugin", 1) &&
                    runtime.findPlugin("SplineCreationPlugin")->error.find("Missing coordinate table") !=
                            std::string::npos,
            "app.addSplines accepted an entry without a coordinate table");
    require(!runtime.invokeMenu("SplineCreationPlugin", 2) &&
                    runtime.findPlugin("SplineCreationPlugin")->error.find("Point table incomplete") !=
                            std::string::npos,
            "app.addSplines accepted an incomplete cubic segment");
    require(!runtime.invokeMenu("SplineCreationPlugin", 3) &&
                    runtime.findPlugin("SplineCreationPlugin")->error.find("finite numbers") != std::string::npos,
            "app.addSplines accepted a nonnumeric coordinate");
    require(runtime.invokeMenu("SplineCreationPlugin", 4) && addedStrokes.size() == 1 &&
                    addedStrokePolicy == prototype::PluginStrokeUndoPolicy::Grouped,
            "app.addSplines did not default to one grouped undo action");
    require(!runtime.invokeMenu("SplineCreationPlugin", 5) &&
                    runtime.findPlugin("SplineCreationPlugin")->error.find("Unrecognized undo option") !=
                            std::string::npos,
            "app.addSplines accepted an unknown undo policy");
    require(!runtime.invokeMenu("StrokeApiPlugin", 1) &&
                    runtime.findPlugin("StrokeApiPlugin")->error.find("There is no selection") != std::string::npos,
            "app.getStrokes selection errors were not reported to the plugin");
    require(runtime.invokeMenu("StrokeMutationPlugin", 0) && addedStrokes.size() == 2 &&
                    addedStrokePolicy == prototype::PluginStrokeUndoPolicy::Individual &&
                    addedStrokes[0].x == std::vector<double>({1, 2}) &&
                    addedStrokes[0].y == std::vector<double>({3, 4}) &&
                    addedStrokes[0].pressure == std::vector<double>({0.5, 0.75}) &&
                    addedStrokes[0].tool == std::optional<std::string>("pen") &&
                    addedStrokes[0].width == std::optional<double>(2.5) &&
                    addedStrokes[0].color == std::optional<std::uint32_t>(0x123456) &&
                    addedStrokes[0].fill == std::optional<int>(-1) &&
                    addedStrokes[0].lineStyle == std::optional<std::string>("plain") &&
                    addedStrokes[1].pressure.empty() &&
                    addedStrokes[1].tool == std::optional<std::string>("highlighter") &&
                    pageWidthChange == 50 && pageHeightChange == 60 && pageSizeRelative && !layerVisible &&
                    renamedLayer == "moved for fitting" && activatedActions.back() == "layer-new-above-current",
            "FitToContent stroke, page-size, and layer APIs did not preserve arguments and references");
    activatedActions.clear();
    require(!runtime.invokeMenu("StrokeMutationPlugin", 1) &&
                    runtime.findPlugin("StrokeMutationPlugin")->error.find("not equal length") != std::string::npos,
            "app.addStrokes accepted malformed point arrays");
    require(runtime.invokeMenu("StrokeMutationPlugin", 2) && exportCalled && exportRequests.size() == 1 &&
                    exportRequests.back().outputFile == "/tmp/plugin-test.png" &&
                    exportRequests.back().range == std::optional<std::string>("2-3; 7") &&
                    exportRequests.back().layerRange == std::optional<std::string>("1-2") &&
                    exportRequests.back().background == "none" && exportRequests.back().progressiveMode &&
                    exportRequests.back().backend == "cairo" && exportRequests.back().pngDpi == 180 &&
                    exportRequests.back().pngWidth == 400 && exportRequests.back().pngHeight == 600,
            "app.export or getDocumentStructure did not preserve the plugin options");
    require(runtime.invokeMenu("StrokeMutationPlugin", 3) && exportRequests.size() == 2 &&
                    exportRequests.back().outputFile == "/tmp/plugin-default.pdf" &&
                    !exportRequests.back().range && !exportRequests.back().layerRange &&
                    exportRequests.back().background == "all" && !exportRequests.back().progressiveMode &&
                    exportRequests.back().backend == "default" && exportRequests.back().pngDpi == -1 &&
                    exportRequests.back().pngWidth == -1 && exportRequests.back().pngHeight == -1,
            "app.export did not apply the master option defaults");
    require(runtime.invokeMenu("QuickScreenshot", 0) && quickScreenshotCalled,
            "Bundled QuickScreenshot did not dispatch app.quickScreenshot to the GTK4 host");
    require(runtime.invokeMenu("BeamerPresentation", 0) && activatedActions == std::vector<std::string>{"beamer-next"},
            "Beamer +1 did not dispatch the next-animation host action");
    require(runtime.invokeMenu("BeamerPresentation", 1) &&
                    activatedActions == std::vector<std::string>({"beamer-next", "beamer-previous"}),
            "Beamer -1 did not dispatch the previous-animation host action");
    require(runtime.invokeMenu("BeamerPresentation", 2) &&
                    activatedActions == std::vector<std::string>({"beamer-next", "beamer-previous", "beamer-clean"}),
            "Beamer clean did not dispatch the cleanup host action");
    require(runtime.invokeMenu("LayerActions", 0) &&
                    activatedActions.back() == "layer-actions-clone-next",
            "LayerActions clone did not dispatch its GTK host action");
    require(runtime.invokeMenu("LayerActions", 1) && activatedActions.back() == "layer-actions-hide",
            "LayerActions hide did not dispatch its GTK host action");
    require(runtime.invokeMenu("LayerActions", 2) && activatedActions.back() == "layer-actions-add-top",
            "LayerActions add did not dispatch its GTK host action");
    require(runtime.invokeMenu("DialogPlugin", 0) && dialogPluginName == "DialogPlugin" &&
                    dialogMessage == "Choose one" && !dialogError && dialogButtons.size() == 2 &&
                    dialogButtons[0].response == 1 && dialogButtons[0].label == "First" &&
                    dialogButtons[1].response == 2 && dialogButtons[1].label == "Second" && dialogResponse,
            "app.openDialog did not preserve its message, ordered buttons, plugin name, and response callback");
    dialogResponse(0);
    require(changedMode == 7, "Closing a plugin dialog must not invoke its button callback");
    dialogResponse(2);
    require(changedMode == 2, "Plugin dialog button responses must reach the Lua callback as integers");
    require(runtime.invokeMenu("DialogPlugin", 1) && dialogError && dialogMessage == "Problem" &&
                    dialogButtons.size() == 1 && dialogButtons[0].response == 1 &&
                    dialogButtons[0].label == "OK" && dialogResponse,
            "Error dialogs must preserve their error flag and button response data");
    dialogResponse(1);
    require(changedMode == 2, "A plugin dialog without a callback must not activate unrelated plugin actions");
    require(runtime.handleKeyPress(GDK_KEY_R, GDK_SHIFT_MASK, false) && changedMode == 7,
            "Plugin canvas shortcut did not run with legacy shifted-key matching");
    require(!runtime.handleKeyPress(GDK_KEY_r, GdkModifierType(0), true),
            "Plugin shortcuts must not steal keyboard input from text editing");

    auto* actionGroup = g_simple_action_group_new();
    auto* menu = g_menu_new();
    require(runtime.appendMenuSections(menu, G_ACTION_MAP(actionGroup), nullptr) == 87 &&
                    g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 24,
            "Loaded plugin menus were not exposed as a named menu section");
    g_object_unref(menu);
    g_object_unref(actionGroup);

    runtime.resetPendingStates();
    runtime.setPendingEnabled("DefaultPlugin", false);
    const auto [enabledPaths, disabledPaths] = runtime.pendingPluginPaths();
    require(enabledPaths.find(std::filesystem::absolute(colorCycle).string()) != std::string::npos &&
                    disabledPaths.find(std::filesystem::absolute(defaultPlugin).string()) != std::string::npos,
            "Plugin manager persistence did not serialize deviations from the default enablement state");
    require(runtime.applyPendingStates() && !runtime.findPlugin("DefaultPlugin")->enabled,
            "Applying plugin manager state did not update the desired enablement state");

    std::filesystem::remove_all(root);
    return 0;
}
