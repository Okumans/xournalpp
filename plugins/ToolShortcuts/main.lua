-- Fast canvas-scoped shortcuts for common tools and page navigation.
--
-- The shortcut option is intentionally different from accelerator: it is handled by the drawing canvas after text
-- editing/input handling, so one-letter shortcuts do not steal characters from a text object.

local toolShortcuts = {
  { menu = "Text (T)", shortcut = "t", tool = app.C.Tool_text },
  { menu = "Smart Select (S)", shortcut = "s", tool = app.C.Tool_smartSelect },
  { menu = "Pen (P)", shortcut = "p", tool = app.C.Tool_pen },
  { menu = "Eraser (E)", shortcut = "e", tool = app.C.Tool_eraser },
  { menu = "Highlighter (H)", shortcut = "h", tool = app.C.Tool_highlighter },
  { menu = "Image (I)", shortcut = "i", tool = app.C.Tool_image },
}

function selectTool(tool)
  app.changeActionState("select-tool", tool)
end

function nextPage()
  app.activateAction("goto-next")
end

function previousPage()
  app.activateAction("goto-previous")
end

function initUi()
  for _, entry in ipairs(toolShortcuts) do
    app.registerUi({
      menu = entry.menu,
      callback = "selectTool",
      mode = entry.tool,
      shortcut = entry.shortcut,
    })
  end

  app.registerUi({
    menu = "Next Page (Shift+J)",
    callback = "nextPage",
    shortcut = "<Shift>j",
  })
  app.registerUi({
    menu = "Previous Page (Shift+K)",
    callback = "previousPage",
    shortcut = "<Shift>k",
  })
end
