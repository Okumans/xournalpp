function initUi()
  app.registerUi({ ["menu"] = "QuickScreenshot", ["callback"] = "Go", ["accelerator"] = "<Shift><Alt>t" })
end

function Go()
  app.quickScreenshot()
end
