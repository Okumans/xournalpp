local m = {
    dec = 1,
    inc = 2,
}
-- Register all Toolbar actions and intialize all UI stuff
function initUi()
    app.registerUi({menu = "beamer +1", callback = "Beamer", mode=m.inc, accelerator="<Alt>y"});
    app.registerUi({menu = "beamer -1", callback = "Beamer", mode=m.dec, accelerator="<Alt>x"});
    app.registerUi({menu = "beamer clean", callback = "BeamerClean"});
end

function Beamer(mode)
    app.activateAction((mode == m.inc) and "beamer-next" or "beamer-previous")
end

function BeamerClean()
    app.activateAction("beamer-clean")
end
