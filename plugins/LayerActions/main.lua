-- Register all Toolbar actions and initialize all UI stuff
function initUi()
  app.registerUi({["menu"] = "Clone non-background layers to next page", ["callback"] = "clone", ["accelerator"]="<Control><Shift>c"});
  app.registerUi({["menu"] = "Hide all layers except first layers and backgrounds", ["callback"] = "hide"});
  app.registerUi({["menu"] = "Add new top layer on each page", ["callback"] = "add"});
end

function clone()
  app.activateAction("layer-actions-clone-next")
end

function hide()
  app.activateAction("layer-actions-hide")
end

function add()
  app.activateAction("layer-actions-add-top")
end
