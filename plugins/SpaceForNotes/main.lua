local m = {
    NORMAL = 0,
    FORCE = 1,
}

-- Register all Toolbar actions and intialize all UI stuff
function initUi()
  app.registerUi{menu="Add one empty pages after every pdf page once", callback="AddEmpty", mode=m.NORMAL}
  app.registerUi{menu="Add empty pages after every page", callback="AddEmpty", mode=m.FORCE}
end

function AddEmpty(mode)
    app.addEmptyPages(mode == m.FORCE)
end
