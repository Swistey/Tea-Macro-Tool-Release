(async () => {
  function check(value, message) { if (!value) throw new Error(message); }
  function editEvent(name, value) {
    const control = elements['event-fields'].querySelector(`[name="${name}"]`);
    check(control, `missing event field: ${name}`);
    control.value = value;
    control.dispatchEvent(new Event('input'));
    control.dispatchEvent(new Event('change'));
  }
  async function until(condition) {
    const limit = Date.now() + 3000;
    while (!await condition()) {
      if (Date.now() > limit) throw new Error('ui operation timed out');
      await new Promise(resolve => setTimeout(resolve, 10));
    }
  }
  async function dismissDeletion(dismiss = () => elements['cancel-deletion'].click()) {
    const closed = new Promise(resolve => elements['delete-confirmation'].addEventListener('close', resolve, { once: true }));
    await dismiss();
    await closed;
  }
  function pointer(type, point, modifiers = 0) {
    return request('test-input', { input: { type, x: point.x, y: point.y, modifiers,
      button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: type === 'mouseMoved' ? 0 : 1 } });
  }
  function center(element) {
    const rect = element.getBoundingClientRect();
    return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
  }
  function menuArrowVisible(menu, edge) {
    const arrow = getComputedStyle(menu, edge);
    return arrow.content !== 'none' && arrow.visibility === 'visible';
  }
  async function pressKey(key, code, windowsVirtualKeyCode) {
    for (const type of ['keyDown', 'keyUp']) await request('test-input', { key: true,
      input: { type, key, code, windowsVirtualKeyCode, text: type === 'keyDown' && key === 'Enter' ? '\r' : '' } });
  }
  try {
    await document.fonts.ready;
    check(document.fonts.check('12px "JetBrains Mono"'), 'bundled font did not load');
    check(getComputedStyle(elements['macro-count']).borderInlineStartWidth === '1px', 'the home macro count is missing its divider');
    check(!selectedKeys.size && !elements.keyboard.querySelector('.selected, [aria-pressed="true"]'), 'startup preselected a keyboard key');
    check(!document.querySelector('.details').textContent.includes('selected keys'), 'the home screen still has a selected keys heading');
    check(elements.assign.disabled && elements['selected-name'].textContent === '0 keys selected' && elements['macro-title'].textContent === 'select a key', 'startup does not show an empty key selection');
    const frame = await request('test-window');
    check(frame.icon && frame.taskbarIcon && !frame.captionIcon, `taskbar icon was removed or the title bar icon remains: ${JSON.stringify(frame)}`);
    check(frame.title === 'Tea, Macro Tool', 'taskbar preview does not use the requested name');
    check(frame.captionSupported ? frame.caption === 0 && frame.captionText === 0 : frame.dark, 'title bar is not blank and black');
    for (const captionActive of [true, false, true, false]) {
      const unfocused = await request('test-window', { captionActive });
      check(unfocused.activeCaption && unfocused.focusUnchanged, 'title bar changed appearance or stole focus on deactivation');
      check(unfocused.captionSupported ? unfocused.caption === 0 : unfocused.dark, 'inactive title bar lost its black styling');
    }
    check(frame.minWidth > 0 && frame.minHeight > 0 && frame.minWidth <= frame.workWidth && frame.minHeight <= frame.workHeight,
      'native minimum window size does not fit the monitor work area');
    for (let edge = 1; edge <= 8; ++edge) {
      const proposed = await request('test-window', { edge });
      check(proposed.handled && proposed.right - proposed.left >= proposed.minWidth && proposed.bottom - proposed.top >= proposed.minHeight, `drag edge ${edge} bypassed the minimum window size`);
      const left = [1, 4, 7].includes(edge), top = [3, 4, 5].includes(edge);
      check((left ? proposed.right === 420 : proposed.left === 100) && (top ? proposed.bottom === 340 : proposed.top === 100), `drag edge ${edge} moved the anchored edge`);
    }
    for (const [width, height] of [[320, 240], [800, 900], [1440, 500]]) {
      const resized = await request('test-window', { width, height });
      check(resized.windowWidth >= resized.minWidth && resized.windowHeight >= resized.minHeight, 'native resize bypassed the minimum window size');
    }
    const bypassed = await request('test-window', { width: 320, height: 240, skipChanging: true });
    check(bypassed.windowWidth >= bypassed.minWidth && bypassed.windowHeight >= bypassed.minHeight, 'post-resize validation allowed undersized content');
    check((await request('test-window', { minimize: true })).minimized, 'minimum-size enforcement prevents minimizing');
    const fullWindow = await request('test-window', { restore: true });
    check(!fullWindow.minimized && fullWindow.windowWidth >= fullWindow.minWidth && fullWindow.windowHeight >= fullWindow.minHeight, 'restoring bypassed the minimum window size');
    await until(() => Math.abs(innerWidth - fullWindow.width / devicePixelRatio) < 2);
    check(getComputedStyle(elements['stop-hotkey']).color === 'rgb(255, 100, 100)' && getComputedStyle(elements['stop-hotkey']).borderTopColor === 'rgb(255, 100, 100)', 'stop hotkey button is not red');
    const browser = await request('test-browser-settings');
    check(!browser.accelerators && !browser.devTools && !browser.zoom, 'browser refresh, find, print, developer tools, or zoom shortcuts remain enabled');
    check(state.stopKey, 'startup did not provide a default stop key');
    if (!state.macros.length) check(state.stopKey === 27, 'new or unconfigured installations do not default to escape');
    check(!document.getElementById('toggle-enabled') && !document.getElementById('stop') && !document.getElementById('capture'), 'removed playback or assignment capture controls remain');
    check(elements['stop-playback'].hidden, 'stop playback is visible when idle');
    check(state.enabled, 'a saved listener pause prevents new playback');
    if (state.macros[0]?.id === 999) {
      check(state.bindings[0]?.macroId === 999 && state.macros[0].actions[0].text === 'persisted', 'saved configuration did not reload');
      if (state.bindingOptions) check(state.bindingOptions.modifiers === 5 && state.bindingOptions.replace, 'checkbox preferences did not reload');
      if (state.layout) check(state.layout === '75%' && elements.layout.value === '75%' && state.stopKey === 120, 'layout or stop hotkey did not reload');
      if (state.recordKey) check(state.recordKey === 121 && (!('clickPosition' in state.recordingOptions) || (state.recordingOptions.clickPosition && state.recordingOptions.mouseButtons && !state.recordingOptions.keyboard)), 'record hotkey or recording settings did not reload');
    }
    if (!elements['save-recovered'].hidden) {
      let accepted = false;
      try { await request('save', { state: nextState() }); accepted = true; } catch {}
      check(!accepted && !elements['save-recovered'].hidden, 'unrelated save confirmed recovery');
      const waiting = state.bindings.find(binding => state.macros.find(macro => macro.id === binding.macroId)?.actions.every(action => action.type === 'wait'));
      if (waiting) {
        await request('test-trigger', { code: waiting.code, modifiers: waiting.modifiers });
        await new Promise(resolve => setTimeout(resolve, 60));
        check(!playback.active, 'recovered macros played before confirmation');
      }
      elements['save-recovered'].click();
      await until(() => elements['save-recovered'].hidden);
    }
    await commit({ version: 1, enabled: true, stopKey: 27, macros: [], bindings: [] });
    await commit({ version: 1, enabled: true, stopKey: 0, macros: [], bindings: [] });
    check(elements.assign.disabled && elements['new-macro'].disabled, 'macros are available without a stop hotkey');
    openEditor();
    check(flowStep === null && elements['macro-flow'].hidden, 'macro creation opened without a stop hotkey');
    let blocked = false;
    try {
      const next = nextState(); next.macros.push({ id: 1, name: 'blocked', actions: [{ type: 'wait', duration: 0 }] });
      await request('save', { state: next });
    } catch { blocked = true; }
    check(blocked, 'native backend allowed creating macros without a stop hotkey');
    blocked = false;
    try { await request('test-trigger', { code: 112 }); } catch { blocked = true; }
    check(blocked, 'native backend allowed playback without a stop hotkey');
    const minimum = await request('test-window', { minimum: true });
    await until(() => Math.abs(innerWidth - minimum.width / devicePixelRatio) < 2);
    const panel = document.querySelector('.details').getBoundingClientRect();
    for (const [layout, count] of [['60%', 61], ['65%', 67], ['75%', 80], ['TKL', 87], ['100%', 104]]) {
      elements.layout.value = layout;
      elements.layout.dispatchEvent(new Event('change'));
      await optionSave;
      check(keyButtons.size === count, `${layout} has the wrong keys`);
      check((await request('state')).layout === layout, 'form factor was not saved');
      const next = document.querySelector('.details').getBoundingClientRect();
      check(Math.abs(next.width - panel.width) < .1 && Math.abs(next.height - panel.height) < .1, 'details panel changed size');
      check(elements.keyboard.scrollWidth <= elements.keyboard.clientWidth + 1, `${layout} scrolls horizontally`);
      check(document.documentElement.scrollWidth <= innerWidth + 1, `${layout} overflows the minimum window width`);
      check(!selectedKeys.size && !elements.keyboard.querySelector('.selected, [aria-pressed="true"]'), `${layout} preselected a keyboard key`);
      const keyboard = elements['keyboard-panel'].getBoundingClientRect();
      check(next.left >= keyboard.right || next.top >= keyboard.bottom, `${layout} overlaps the details panel at minimum size`);
    }
    const restoredWindow = await request('test-window', { restore: true });
    await until(() => Math.abs(innerWidth - restoredWindow.width / devicePixelRatio) < 2);

    const stopPicker = elements['stop-picker'];
    elements['stop-hotkey'].click();
    await until(() => hotkeyCapture);
    const firstCaptureId = hotkeyCapture.id;
    let cancellationReceived = false;
    function observeCancellation({ data }) {
      if (data.event === 'captured' && data.captureId === firstCaptureId && data.trigger === null) cancellationReceived = true;
    }
    native.addEventListener('message', observeCancellation);
    const firstClose = new Promise(resolve => stopPicker.addEventListener('close', resolve, { once: true }));
    elements['close-stop'].click();
    await firstClose;
    check(!hotkeyCapture && !state.stopKey, 'cancelling stop key selection did not clear the capture');
    elements['stop-hotkey'].click();
    await until(() => cancellationReceived);
    native.removeEventListener('message', observeCancellation);
    check(hotkeyCapture && hotkeyCapture.id !== firstCaptureId && stopPicker.open,
      'the previous cancellation closed the reopened stop hotkey picker');
    const stopClosed = new Promise(resolve => stopPicker.addEventListener('close', resolve, { once: true }));
    await request('test-trigger', { code: 27 });
    await until(() => state.stopKey === 27 && !saving);
    await stopClosed;
    check(elements['stop-hotkey'].textContent === 'stop hotkey: esc' && !elements['stop-picker'].open, 'stop hotkey capture did not save or close');
    check(elements.assign.disabled && !elements['new-macro'].disabled, 'setting the stop key did not enable creation while keeping assignment unavailable without selected keys');
    openEditor();
    elements['macro-name'].value = 'recording draft';
    elements['macro-name-form'].requestSubmit();
    document.querySelector('[data-macro-type="no-repeat"]').click();
    check(!state.recordKey && !elements.editor.querySelector('#record-events') && !elements['record-status'].textContent,
      'the events screen still shows an idle recording box');
    addAction(defaultEvent('wait'));
    addAction(defaultEvent('wait'));
    selectEvent(0);
    elements['add-event'].click();
    for (const option of ['mouse click', 'record inputs']) {
      elements['add-event-menu'].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true }));
      check(document.activeElement.textContent === option, `add event keyboard navigation skipped ${option}`);
    }
    document.activeElement.click();
    await until(() => recording && !recordingPending);
    check(elements['add-event-menu'].hidden && elements['record-status'].textContent.includes('press esc to stop'),
      'menu recording did not close the picker or show the stop key');
    await request('test-record', { key: 65, down: true, time: 100 });
    await request('test-record', { key: 65, down: false, time: 125 });
    await request('test-record', { key: 27, down: true, time: 130 });
    await until(() => !recording && !recordingPending);
    check(editorActions.map(action => action.type).join() === 'wait,wait,key' && editorActions[2].key === 65,
      'menu recording did not append to the chosen sequence or captured the stop key');
    await closeEditor();
    elements['record-hotkey'].click();
    await until(() => hotkeyCapture?.kind === 'record');
    const rejectedRecordCapture = hotkeyCapture;
    await request('test-trigger', { code: 27 });
    await until(() => hotkeyCapture && hotkeyCapture !== rejectedRecordCapture &&
      elements.toast.textContent.includes('different from the stop hotkey'));
    check(!state.recordKey, 'record capture accepted the stop hotkey');
    await request('test-trigger', { code: 121 });
    await until(() => state.recordKey === 121 && !saving);
    check(elements['record-hotkey'].textContent === 'record hotkey: f10', 'the home page does not display the configured record hotkey');
    elements.toast.hidden = true;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { page: 'home' });
    selectedKeys.clear(); selectedKeys.add('F10'); renderSelection();
    check(elements.assign.disabled, 'record hotkey remains assignable to a macro');
    selectedKeys.clear(); selectedKeys.add('K'); renderSelection();
    showMacros();
    elements['new-macro'].click();
    check(flowStep === 'name' && elements['keyboard-page'].hidden && !elements['macro-name-page'].hidden && elements.editor.hidden, 'creation did not open the name page');
    check(!document.querySelector('dialog[open]'), 'macro creation is still displayed in a popup');
    elements['macro-name'].value = '   ';
    elements['macro-name-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(flowStep === 'name', 'blank name advanced to the next page');
    elements['macro-name'].value = '\u00e9'.repeat(65);
    elements['macro-name-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(flowStep === 'name', 'oversized UTF-8 name advanced to the next page');
    elements['macro-name'].value = 'Smoke Macro';
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { page: 'name' });
    await pressKey('Enter', 'Enter', 13);
    check(flowStep === 'type' && elements['macro-name-page'].hidden && !elements['macro-type-page'].hidden && elements.editor.hidden, 'name did not advance to the type page');
    check(document.activeElement === elements['type-title'] && getComputedStyle(elements['type-title']).outlineStyle === 'none',
      'pressing enter left a focus outline on the macro type heading');
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { page: 'type' });
    elements['flow-back'].click();
    check(flowStep === 'name' && elements['macro-name'].value === 'Smoke Macro', 'back navigation lost the draft name');
    elements['macro-name-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    document.querySelector('[data-macro-type="toggle"]').click();
    check(flowStep === 'events' && elements['macro-type-page'].hidden && !elements.editor.hidden && elements['editor-type'].textContent === 'toggle', 'choosing a type did not open the creation page');
    check(!editorActions.length && !elements.steps.querySelector('.event-block') && elements.steps.lastElementChild === elements['event-adder'], 'new macro does not start with an empty sequence and an add button');
    const eventInspector = document.querySelector('.event-inspector');
    check(eventInspector.hidden, 'an empty macro showed the event fields');
    addAction(defaultEvent('wait'));
    check(!eventInspector.hidden && elements['event-fields'].querySelector('[name="duration"]'), 'the first event did not show its fields');
    removeEvent();
    check(eventInspector.hidden && !elements['event-fields'].children.length, 'removing the last event left its fields visible');
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { page: 'unselected' });
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(flowStep === 'events' && !state.macros.length, 'an empty sequence was saved');
    elements['add-event'].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true }));
    check(!elements['add-event-menu'].hidden && document.activeElement.dataset.addEvent === 'key', 'add event menu did not open with the keyboard');
    elements['add-event-menu'].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true }));
    check(document.activeElement.dataset.addEvent === 'mouse', 'menu arrow navigation did not move focus');
    elements['add-event-menu'].dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    check(elements['add-event-menu'].hidden && document.activeElement === elements['add-event'], 'escape did not dismiss the menu and return focus');
    for (const [type, fields] of [
      ['text', { text: 'Hello, Tea!' }], ['key', { key: 67, modifiers: 2 }],
      ['mouse', { button: 1 }], ['wait', { duration: 10 }], ['move', { x: -10, y: 20 }]
    ]) {
      elements['add-event'].click();
      document.querySelector(`[data-add-event="${type}"]`).click();
      check(elements['add-event-menu'].hidden, 'choosing an event left its menu open');
      for (const [name, value] of Object.entries(fields)) editEvent(name, value);
    }
    selectEvent(0);
    const delayType = elements['event-fields'].querySelector('[name="delayType"]');
    check(delayType?.value === 'manual' && elements['event-fields'].querySelector('[name="duration"]').value === '0',
      'type text did not show its manual delay by default');
    editEvent('duration', 35);
    editEvent('delayType', 'random');
    check(elements['event-fields'].querySelector('[name="minDelay"]').value === '35' &&
      elements['event-fields'].querySelector('[name="maxDelay"]').value === '35', 'random delay did not open with the manual value as both bounds');
    editEvent('delayType', 'manual');
    check(!editorActions[0].delayRange && elements['event-fields'].querySelector('[name="duration"]').value === '35',
      'switching to manual did not restore the text delay');
    editEvent('delayType', 'random');
    editEvent('minDelay', 45);
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(selectedEvent === 0 && elements.toast.textContent.includes('minimum at or below the maximum'), 'an inverted text delay range was accepted');
    editEvent('minDelay', 25);
    selectEvent(1);
    const holdType = elements['event-fields'].querySelector('[name="holdType"]');
    check(holdType instanceof HTMLSelectElement && holdType.labels[0].textContent.startsWith('hold type') &&
      [...holdType.options].map(option => option.textContent).join(', ') === 'manual hold duration, random hold duration',
      'hold type did not show both choices in a labeled dropdown');
    check(holdType.value === 'manual' && elements['event-fields'].querySelector('[name="duration"]').value === '0',
      'the manual duration field was hidden by default');
    holdType.scrollIntoView({ block: 'center' });
    const holdLabelBounds = holdType.labels[0].getBoundingClientRect();
    const holdLabelPoint = { x: holdLabelBounds.left + 8, y: holdLabelBounds.top + 8 };
    let openedHoldPicker = false;
    holdType.showPicker = () => { openedHoldPicker = true; };
    await pointer('mousePressed', holdLabelPoint);
    await pointer('mouseReleased', holdLabelPoint);
    delete holdType.showPicker;
    check(openedHoldPicker && document.activeElement === holdType, 'clicking the hold type label did not open its picker');
    editEvent('duration', 25);
    editEvent('holdType', 'random');
    check(document.activeElement.name === 'holdType' && !elements['event-fields'].querySelector('[name="duration"]') &&
      elements['event-fields'].querySelector('[name="minHoldDuration"]') && elements['event-fields'].querySelector('[name="maxHoldDuration"]'),
      'random hold did not replace the single field with the range');
    editEvent('minHoldDuration', 50);
    editEvent('maxHoldDuration', 40);
    selectEvent(2);
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(selectedEvent === 1 && elements.toast.textContent.includes('minimum at or below the maximum') &&
      elements['event-fields'].querySelector('[name="minHoldDuration"]'), 'saving did not select the event with the invalid hold range');
    editEvent('minHoldDuration', 10);
    selectEvent(2);
    editEvent('holdType', 'random');
    editEvent('maxHoldDuration', 30);
    editEvent('mode', 'down');
    check(!elements['event-fields'].querySelector('[name="holdType"]') && elements['event-fields'].querySelector('[name="duration"]'),
      'a split mouse event showed the hold menu instead of its delay');
    editEvent('mode', 'press');
    editEvent('holdType', 'manual');
    editEvent('duration', 15);
    check(!editorActions[2].holdRange && !elements['event-fields'].querySelector('[name="maxHoldDuration"]'),
      'switching the mouse click to manual left the random range active');
    selectEvent(4);
    check(elements.steps.querySelectorAll('.event-block').length === 5, 'palette did not create event blocks');
    const selectedVisual = elements.steps.querySelector('.event-block[aria-pressed="true"] .event-visual');
    check(getComputedStyle(selectedVisual).outlineStyle === 'none', 'the selected event has an outline');
    elements['event-fields'].querySelector('[name="x"]').dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    check(selectedEvent === 4 && !eventInspector.hidden, 'clicking a property field deselected its event');
    elements['editor-title'].dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    check(selectedEvent === -1 && !eventInspector.hidden && elements['event-fields'].querySelector('[name="x"]').value === '-10',
      'clicking the page background hid or changed the event fields');
    editEvent('x', -12);
    check(editorActions[4].x === -12, 'the event fields could not be edited after clicking the background');
    editEvent('x', -10);
    const originalOrder = JSON.stringify(editorActions);
    selectEvent(0);
    elements['add-event'].click();
    document.querySelector('[data-add-event="move"]').click();
    editEvent('x', -99);
    check(editorActions[4].x === -10 && editorActions[5].x === -99, 'editing an event changed an unrelated event');
    check(elements.steps.lastElementChild === elements['event-adder'] && JSON.stringify(editorActions.slice(0, 5)) === originalOrder, 'adding an event did not append it before the add button');
    const backspace = new KeyboardEvent('keydown', { key: 'Backspace', bubbles: true, cancelable: true });
    elements.steps.querySelector('[data-index="5"]').dispatchEvent(backspace);
    check(backspace.defaultPrevented && JSON.stringify(editorActions) === originalOrder, 'backspace did not delete only the targeted block');
    selectEvent(0);
    const typingBackspace = new KeyboardEvent('keydown', { key: 'Backspace', bubbles: true, cancelable: true });
    elements['event-fields'].querySelector('textarea').dispatchEvent(typingBackspace);
    check(!typingBackspace.defaultPrevented && editorActions.length === 5, 'backspace in a text field deleted an event');
    check(!elements.steps.querySelector('.event-number'), 'event numbers remain in the editor');
    elements['flow-back'].click();
    check(flowStep === 'type' && macroType === 'toggle', 'back navigation lost the macro type');
    document.querySelector('[data-macro-type="toggle"]').click();
    check(JSON.stringify(editorActions) === originalOrder, 'page navigation lost the event sequence');
    const transfer = new DataTransfer();
    elements.steps.querySelector('[data-index="2"]').dispatchEvent(new DragEvent('dragstart', { dataTransfer: transfer, bubbles: true }));
    elements.steps.querySelector('[data-index="4"]').dispatchEvent(new DragEvent('drop', { dataTransfer: transfer, bubbles: true, cancelable: true }));
    const expectedForward = JSON.parse(originalOrder);
    expectedForward.splice(3, 0, expectedForward.splice(2, 1)[0]);
    check(JSON.stringify(editorActions) === JSON.stringify(expectedForward) && selectedEvent === 3, 'forward drag did not insert before the highlighted event');
    elements.steps.querySelector('[data-index="3"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', altKey: true, bubbles: true, cancelable: true }));
    elements.steps.querySelector('[data-index="4"]').dispatchEvent(new DragEvent('dragstart', { dataTransfer: transfer, bubbles: true }));
    elements.steps.querySelector('[data-index="3"]').dispatchEvent(new DragEvent('drop', { dataTransfer: transfer, bubbles: true, cancelable: true }));
    check(editorActions[3].type === 'move' && selectedEvent === 3, 'drag did not reorder the event');
    elements.steps.querySelector('[data-index="3"]').dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', altKey: true, bubbles: true, cancelable: true }));
    check(JSON.stringify(editorActions) === originalOrder, 'moving an event changed its properties');
    selectEvent(3);
    editEvent('duration', -1);
    selectEvent(0);
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    check(flowStep === 'events' && selectedEvent === 3 && !state.macros.length, 'invalid unselected event was saved');
    editEvent('duration', 10);
    check(elements.editor.scrollWidth <= elements.editor.clientWidth + 1, 'block editor overflows horizontally');
    const savedDraft = JSON.stringify(editorActions);
    const savedSelection = selectedEvent;
    const shortPageHeight = document.documentElement.scrollHeight;
    for (let index = 0; index < 48; ++index) addAction({ type: 'key', key: 72, mode: index % 2 ? 'up' : 'down', modifiers: 0, duration: 0 });
    const longPageHeight = document.documentElement.scrollHeight;
    check(longPageHeight > shortPageHeight && longPageHeight > innerHeight, 'adding events did not grow the page');
    check(elements.steps.scrollHeight <= elements.steps.clientHeight + 1, 'events are confined to an internal scrolling area');
    const lastEvent = elements.steps.querySelector(`[data-index="${selectedEvent}"]`);
    lastEvent.scrollIntoView({ block: 'end' });
    check(scrollY > 0 && lastEvent.getBoundingClientRect().bottom <= innerHeight + 1, 'page scrolling cannot reach the last event');
    elements['add-event'].scrollIntoView({ block: 'end' });
    elements['add-event'].click();
    const addMenu = elements['add-event-menu'];
    const addMenuBounds = addMenu.getBoundingClientRect();
    check(addMenuBounds.left >= 0 && addMenuBounds.right <= innerWidth && addMenuBounds.top >= 0 && addMenuBounds.bottom <= innerHeight, 'the trailing add menu opens outside the viewport');
    check(!menuArrowVisible(addMenu, '::before') && !menuArrowVisible(addMenu, '::after'), 'a menu with all options visible showed scroll arrows');
    addMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    for (let index = 0; index < 48; ++index) { selectedEvent = savedSelection + 1; removeEvent(); }
    check(JSON.stringify(editorActions) === savedDraft, 'removing events changed unrelated events');
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    check(document.documentElement.scrollHeight < longPageHeight, `removing events did not shrink the page: ${longPageHeight} -> ${document.documentElement.scrollHeight}`);
    selectEvent(savedSelection);
    window.scrollTo(0, 0);
    elements.toast.hidden = true;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview');
    const editorMinimum = await request('test-window', { minimum: true });
    await until(() => Math.abs(innerWidth - editorMinimum.width / devicePixelRatio) < 2);
    selectEvent(1);
    const addButton = elements['add-event'];
    const adder = elements['event-adder'];
    adder.style.top = `${innerHeight / 2 - center(addButton).y}px`;
    addButton.click();
    const menuOptions = addMenu.firstElementChild;
    const maxScroll = menuOptions.scrollHeight - menuOptions.clientHeight;
    check(maxScroll > 0 && !menuArrowVisible(addMenu, '::before') && menuArrowVisible(addMenu, '::after'),
      'a clipped menu did not show only the down arrow at the top');
    const menuBounds = addMenu.getBoundingClientRect();
    const optionsBounds = menuOptions.getBoundingClientRect();
    check(optionsBounds.top > menuBounds.top + 14 && optionsBounds.bottom < menuBounds.bottom - 14,
      'scroll arrows do not have space above and below the options');
    menuOptions.scrollTop = maxScroll / 2;
    await until(() => menuArrowVisible(addMenu, '::before') && menuArrowVisible(addMenu, '::after'));
    addMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true, cancelable: true }));
    await until(() => menuArrowVisible(addMenu, '::before') && !menuArrowVisible(addMenu, '::after'));
    check(document.activeElement.dataset.addEvent === 'move', 'scroll arrows interfered with keyboard navigation');
    addMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true, cancelable: true }));
    await until(() => !menuArrowVisible(addMenu, '::before') && menuArrowVisible(addMenu, '::after'));
    addMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    adder.style.top = '';
    const editorBounds = elements.editor.getBoundingClientRect();
    check(editorBounds.left >= 0 && editorBounds.right <= innerWidth, 'editor overflows the minimum window width');
    elements['save-macro'].scrollIntoView({ block: 'end' });
    check(elements['save-macro'].getBoundingClientRect().bottom <= innerHeight + 1, 'page scrolling cannot reach the save button');
    check(elements['event-fields'].querySelector('[name="maxHoldDuration"]').getBoundingClientRect().bottom <= document.querySelector('.event-inspector').getBoundingClientRect().bottom, 'event properties are clipped at the minimum window size');
    window.scrollTo(0, 0);
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { minimum: true });
    const editorRestored = await request('test-window', { restore: true });
    await until(() => Math.abs(innerWidth - editorRestored.width / devicePixelRatio) < 2);
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    await until(() => state.macros.length === 1 && !saving);
    const macro = state.macros[0];
    check(!elements['macro-list'].querySelector('svg'), 'macro lightning bolt remains');
    check(macro.name === 'smoke macro' && macro.actions.length === 5, 'macro editor did not save every action');
    check(macro.type === 'toggle' && flowStep === null && !elements['keyboard-page'].hidden, 'macro type did not save or creation did not return to the keyboard');
    check(macro.actions[0].text === 'Hello, Tea!', 'text action changed functional character case');
    check(macro.actions[0].delayRange.min === 25 && macro.actions[0].delayRange.max === 35, 'random text delay bounds did not save');
    check(macro.actions[1].holdRange.min === 10 && macro.actions[1].holdRange.max === 40 &&
      macro.actions[2].duration === 15 && !macro.actions[2].holdRange, 'manual and random hold settings did not save');
    check(macro.actions[3].duration === 10 && macro.actions[4].x === -10, 'action values changed');

    openEditor(macro);
    check(editorActions.length === 5 && editorActions[0].text === 'Hello, Tea!', 'saved events did not reopen');
    check(selectedEvent === -1 && !eventInspector.hidden && elements['event-fields'].querySelector('textarea').value === 'Hello, Tea!',
      'reopening a macro did not show the first event fields');
    check(elements['event-fields'].querySelector('[name="delayType"]').value === 'random' &&
      elements['event-fields'].querySelector('[name="minDelay"]').value === '25', 'the saved text delay range did not reopen');
    selectEvent(1);
    check(elements['event-fields'].querySelector('[name="holdType"]').value === 'random' &&
      elements['event-fields'].querySelector('[name="maxHoldDuration"]').value === '40', 'reopening did not display the saved hold range');
    editEvent('minHoldDuration', 12);
    check(macro.actions[1].holdRange.min === 10, 'editing the hold range changed the saved macro');
    editEvent('minHoldDuration', 10);
    selectEvent(0);
    check(!elements['record-split-keys'].checked, 'recording does not combine key down and release by default');
    await request('test-trigger', { code: 121 });
    await until(() => recording && !recordingPending);
    for (const input of [
      { key: 65, down: true, time: 100 }, { key: 65, down: false, time: 125 },
      { key: 66, down: true, time: 145 }, { key: 66, down: false, time: 175 }
    ]) await request('test-record', input);
    await request('test-trigger', { code: 121 });
    await until(() => !recording && editorActions.length === 8);
    check(editorActions[1].mode === 'press' && editorActions[1].duration === 25 && editorActions[2].duration === 20 && editorActions[3].mode === 'press' && editorActions[3].duration === 30, 'combined recording lost key presses, hold durations, or the delay between keys');
    for (let index = 0; index < 3; ++index) { selectedEvent = 1; removeEvent(); }
    selectEvent(0);
    elements['record-split-keys'].checked = true;
    elements['record-split-keys'].dispatchEvent(new Event('change'));
    await optionSave;
    check(state.recordingOptions.splitKeyEvents, 'split key setting did not persist');
    await request('test-trigger', { code: 121 });
    await until(() => recording && !recordingPending);
    check(elements['record-status'].textContent.includes('press esc or f10 to stop'), 'recording does not show the configured stop keys');
    await request('test-preview', { page: 'recording' });
    check(elements['save-macro'].disabled && elements['macro-name'].disabled && elements['add-event'].disabled, 'recording leaves editing enabled');
    for (const input of [
      { key: 162, down: true, time: 100 }, { key: 67, down: true, time: 120 },
      { key: 67, down: false, time: 140 }, { key: 162, down: false, time: 160 }
    ]) await request('test-record', input);
    await request('test-trigger', { code: 121 });
    await until(() => !recording && editorActions.length === 12);
    check(editorActions[1].mode === 'down' && editorActions[1].key === 162 && editorActions[2].duration === 20 && editorActions[7].mode === 'up', 'recording lost input order, timing, or insertion point');
    check(!elements['save-macro'].disabled && !elements['record-status'].textContent, 'record hotkey did not stop recording and unlock the editor');
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    await until(() => flowStep === null && !saving);
    check(state.macros[0].actions[1].mode === 'down' && (await request('state')).macros[0].actions.length === 12, 'recorded events did not persist');
    openEditor(state.macros[0]);
    check(elements['record-split-keys'].checked, 'reopening the editor lost the split key setting');
    selectEvent(0);
    elements['record-settings'].click();
    check(!elements['record-settings-menu'].hidden && document.activeElement === elements['record-delays'], 'recording settings did not open as a keyboard-accessible dropdown');
    elements['record-delays'].checked = false;
    elements['record-delays'].dispatchEvent(new Event('change'));
    await optionSave;
    check(!state.recordingOptions.delays && !elements['record-settings-menu'].hidden, 'recording setting did not persist or unexpectedly closed its menu');
    await request('test-trigger', { code: 121 });
    await until(() => recording && !recordingPending);
    await request('test-record', { mouse: true, key: 1, down: true, time: 200 });
    await request('test-record', { mouse: true, key: 1, down: false, time: 240 });
    await until(() => editorActions.length === 14);
    check(editorActions[1].type === 'mouse' && editorActions[2].mode === 'up', 'mouse recording added delays when disabled');
    elements['close-editor'].click();
    await until(() => !recording && flowStep === null);
    check(state.macros[0].actions.length === 12, 'cancelling recorded edits changed the saved macro');

    openEditor(state.macros[0]);
    elements['record-settings'].click();
    elements['record-mouse'].checked = false;
    elements['record-mouse'].dispatchEvent(new Event('change'));
    await optionSave;
    check(elements['record-position'].disabled && !elements['record-position'].checked, 'click positions are available without mouse click recording');
    elements['record-position'].click();
    check(!elements['record-position'].checked, 'disabled click-position setting was toggleable');
    elements['record-mouse'].checked = true;
    elements['record-mouse'].dispatchEvent(new Event('change'));
    await optionSave;
    check(!elements['record-position'].disabled, 'enabling mouse clicks did not enable the position setting');
    elements['record-position'].checked = true;
    elements['record-keyboard'].checked = false;
    elements['record-position'].dispatchEvent(new Event('change'));
    await optionSave;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await request('test-preview', { page: 'record-settings' });
    await request('test-trigger', { code: 121 });
    await until(() => recording && !recordingPending);
    check(elements['record-settings-menu'].hidden && elements['record-settings'].disabled, 'recording leaves its settings editable');
    await request('test-record', { key: 65, down: true, time: 100 });
    await request('test-record', { mouse: true, key: 0, down: true, x: -10, y: 20, time: 100 });
    await request('test-record', { mouse: true, key: 0, down: false, x: -10, y: 20, time: 110 });
    await request('test-record', { mouse: true, key: 1, down: true, x: 15, y: 25, time: 120 });
    await request('test-record', { mouse: true, key: 1, down: false, x: 15, y: 25, time: 130 });
    await request('test-trigger', { code: 121 });
    await until(() => !recording && editorActions.length === 16);
    check(editorActions[12].type === 'mouse' && editorActions[12].positioned && editorActions[12].x === -10 && editorActions[14].button === 1 && editorActions[14].x === 15 && editorActions[14].y === 25, 'click recording lost mouse buttons or their positions');
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    await until(() => flowStep === null && !saving);
    check(state.macros[0].actions.length === 16 && (await request('state')).recordingOptions.clickPosition, 'positioned clicks or recording settings did not persist');

    showMacros();
    elements['mod-ctrl'].checked = false;
    elements['mod-alt'].checked = true;
    elements['mod-shift'].checked = true;
    elements['replace-input'].checked = true;
    elements['mod-shift'].dispatchEvent(new Event('change'));
    await optionSave;
    elements.macros.close();

    selectedKeys.clear(); selectedKeys.add('Esc'); renderSelection();
    check(elements.assign.disabled && keyButtons.get('Esc').classList.contains('stop-key'), 'stop key remains assignable');
    for (const key of ['F12', 'Left Shift', 'Right Shift', 'Left Ctrl', 'Right Ctrl', 'Left Alt', 'Right Alt', 'Left Win', 'Right Win']) {
      selectedKeys.clear(); selectedKeys.add(key); renderSelection();
      check(elements.assign.disabled, `${key} remains assignable`);
      selectedKeys.add('K'); renderSelection();
      check(elements.assign.disabled, 'mixed selection includes an unsupported key');
    }
    selectedKeys.clear(); selectedKeys.add('K'); renderSelection();
    showMacros();
    check(!elements['mod-ctrl'].checked && elements['mod-alt'].checked && elements['mod-shift'].checked && elements['replace-input'].checked, 'unassigned checkbox preferences did not persist');
    elements.macros.close();

    selectedKeys.clear(); selectedKeys.add('K'); selectedKeys.add('F1'); renderSelection();
    elements['mod-ctrl'].checked = true; elements['mod-alt'].checked = true;
    elements['mod-shift'].checked = false;
    elements['replace-input'].checked = true;
    await assignMacro(macro);
    check(state.bindings.length === 2 && bindings.get('K').modifiers === 3, 'bulk assignment or modifiers failed');
    check(state.bindings.every(binding => binding.replace), 'replacement preference was not saved');
    check(keyButtons.get('K').classList.contains('bound'), 'bound key marker missing');
    check(getComputedStyle(keyButtons.get('K'), '::after').backgroundColor === 'rgb(74, 222, 128)', 'bound marker is not green');
    check((await request('state')).bindings.length === 2, 'native state did not retain assignments');

    selectedKeys.clear(); selectedKeys.add('K'); renderSelection();
    showMacros();
    elements['mod-ctrl'].checked = false;
    elements['mod-alt'].checked = false;
    elements['mod-shift'].checked = true;
    elements['replace-input'].checked = false;
    for (const id of ['mod-ctrl', 'mod-alt', 'mod-shift', 'replace-input']) elements[id].dispatchEvent(new Event('change'));
    await optionSave;
    check(bindings.get('K').modifiers === 4 && !bindings.get('K').replace && bindings.get('K').macroId === macro.id, 'checkbox changes did not update the existing assignment');
    check(bindings.get('F1').modifiers === 3 && bindings.get('F1').replace, 'checkbox changes modified an unselected assignment');
    elements.macros.close();
    showMacros();
    check(!elements['mod-ctrl'].checked && !elements['mod-alt'].checked && elements['mod-shift'].checked && !elements['replace-input'].checked, 'assigned checkbox preferences reset');
    elements.macros.close();

    elements['stop-hotkey'].click();
    await until(() => hotkeyCapture);
    const rejectedStopCapture = hotkeyCapture;
    await request('test-trigger', { code: 112 });
    await until(() => hotkeyCapture && hotkeyCapture !== rejectedStopCapture &&
      elements.toast.textContent.includes('stop hotkey cannot'));
    check(state.stopKey === 27, 'a conflicting stop key replaced the previous key');
    await request('test-trigger', { code: 120 });
    await until(() => state.stopKey === 120 && !saving);
    check(elements['stop-hotkey'].textContent === 'stop hotkey: f9', 'replacement stop hotkey was not displayed');

    const unchanged = JSON.stringify(await request('state'));
    const invalidCases = [
      value => { value.macros[0].actions[0].type = 'unknown'; },
      value => { value.macros[0].actions[0].duration = -1; },
      value => { value.macros[0].actions[0].duration = 1.5; },
      value => { value.bindings[0].code = 4294967361; },
      value => { value.bindings[0].code = 0; },
      value => { value.bindings[0].macroId = 999; },
      value => { value.bindings.push({ ...value.bindings[0] }); },
      value => { value.bindingOptions = { modifiers: 8, replace: false }; },
      value => { value.bindingOptions = { modifiers: 1.5, replace: false }; },
      value => { value.bindingOptions = { modifiers: 0, replace: 'true' }; },
      value => { value.stopKey = 112; },
      value => { value.stopKey = 17; },
      value => { value.stopKey = 1.5; },
      value => { value.stopKey = 512; },
      value => { value.recordKey = value.stopKey; },
      value => { value.recordKey = 112; },
      value => { value.recordingOptions.delays = 'true'; },
      value => { value.recordingOptions.splitKeyEvents = 'true'; },
      value => { value.recordingOptions.mouseButtons = false; },
      value => { value.layout = 'invalid'; },
      value => { value.bindings[0].code = 120; value.bindings[0].modifiers = 2; }
    ];
    for (const change of invalidCases) {
      const invalid = JSON.parse(unchanged); change(invalid);
      let rejected = false;
      try { await request('save', { state: invalid }); } catch { rejected = true; }
      check(rejected, 'native validation accepted an invalid configuration');
      check(JSON.stringify(await request('state')) === unchanged, 'failed save changed existing bindings');
    }

    elements.deselect.click();
    check(!selectedKeys.size && state.bindings.length === 2, 'deselect removed bindings');
    elements.layout.value = '60%'; renderKeyboard();
    check(!selectedKeys.size, 'form factor reselected keys after deselect all');
    keyButtons.get('K').click();
    elements.remove.click();
    await until(() => !saving);
    check(!bindings.has('K') && state.bindings.length === 1,
      `could not remove only the selected assignment. ${elements.toast.textContent}`);
    openEditor(state.macros[0]);
    elements['flow-back'].click();
    elements['flow-back'].click();
    elements['macro-name'].value = 'updated';
    elements['macro-name-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    document.querySelector('[data-macro-type="no-repeat"]').click();
    elements['macro-form'].dispatchEvent(new Event('submit', { cancelable: true }));
    await until(() => state.macros[0]?.name === 'updated' && !saving);
    check(state.bindings[0].macroId === macro.id, 'editing broke existing references');
    check(state.macros[0].type === 'no-repeat', 'editing did not retain the selected macro type');
    renderMacros();
    const beforeDeletion = JSON.stringify(await request('state'));
    elements['macro-list'].querySelector('.negative').click();
    check(elements['delete-confirmation'].open && elements['delete-question'].textContent === 'are you sure you want to delete the macro "updated" and its key assignments?', 'macro deletion did not request confirmation for the macro and its assignments');
    check(document.activeElement === elements['cancel-deletion'] && !saving, 'macro deletion does not default to cancel or started saving before confirmation');
    await dismissDeletion();
    check(JSON.stringify(await request('state')) === beforeDeletion, 'cancelling macro deletion changed saved data');
    elements['macro-list'].querySelector('.negative').click();
    await dismissDeletion(() => pressKey('Escape', 'Escape', 27));
    check(JSON.stringify(await request('state')) === beforeDeletion, 'escaping macro deletion changed saved data');
    elements['macro-list'].querySelector('.negative').click();
    elements['confirm-deletion'].click();
    await until(() => !state.macros.length && !saving);
    check(!state.bindings.length, 'deleting a macro left dangling bindings');

    keyButtons.get('K').focus();
    await pressKey('Tab', 'Tab', 9);
    check(document.activeElement.matches('.key:focus-visible'), 'keyboard navigation lost its key focus indicator');
    const keyboardKey = document.activeElement.dataset.key;
    await pressKey('Enter', 'Enter', 13);
    check(selectedKeys.size === 1 && selectedKeys.has(keyboardKey), 'keyboard activation did not select its focused key');
    check(getComputedStyle(document.activeElement).outlineStyle === 'none' &&
      getComputedStyle(document.activeElement).textDecorationLine === 'underline', 'the selected key did not use an underline for focus');
    const a = center(keyButtons.get('A'));
    await pointer('mousePressed', a);
    await pointer('mouseReleased', a);
    check(selectedKeys.size === 1 && selectedKeys.has('A'), 'pointer click did not select its key');
    check(document.activeElement === keyButtons.get('A') && !keyButtons.get('A').matches(':focus-visible'), 'pointer selection left a keyboard focus outline');
    await Promise.all(elements.keyboard.getAnimations({ subtree: true }).map(animation => animation.finished));
    await request('test-preview', { page: 'home' });
    for (const id of ['D', 'A']) {
      const point = center(keyButtons.get(id));
      const selected = selectedKeys.has(id), count = selectedKeys.size;
      await pointer('mousePressed', point, 2);
      await pointer('mouseReleased', point, 2);
      check(selectedKeys.has(id) !== selected && selectedKeys.size === count + (selected ? -1 : 1), 'ctrl-click did not toggle only its key');
      check(!elements.keyboard.querySelector(':focus-visible'), 'ctrl-click left a keyboard focus outline');
    }

    const bounds = elements['keyboard-panel'].getBoundingClientRect();
    const last = keyButtons.get('D').getBoundingClientRect();
    const start = { x: bounds.left + 20, y: bounds.top + 20 };
    await pointer('mousePressed', start);
    await pointer('mouseReleased', start);
    check(!selectedKeys.size && !elements.keyboard.querySelector(':focus-visible'), 'background click left selected keys or a focus outline');
    await pointer('mousePressed', start);
    await pointer('mouseMoved', { x: last.right, y: last.bottom });
    check(selectedKeys.has('Q') && selectedKeys.has('D'), 'outer-panel drag did not include intersecting keys');
    check(!elements['selection-box'].hidden && elements.layout.className === 'layout-tag', 'drag affected the form factor selector');
    check(getComputedStyle(elements.layout).zIndex > getComputedStyle(elements['selection-box']).zIndex, 'selection covers the form factor selector');
    const pointerId = drag.pointerId;
    await pointer('mouseReleased', { x: bounds.right + 10, y: bounds.bottom + 10 });
    check(!drag && elements['selection-box'].hidden && !elements['keyboard-panel'].hasPointerCapture(pointerId), 'release outside the keyboard left a selection outline or pointer capture');
    for (const type of ['pointercancel', 'lostpointercapture', 'blur', 'layout']) {
      await pointer('mousePressed', a);
      await pointer('mouseMoved', center(keyButtons.get('D')));
      check(drag && !elements['selection-box'].hidden, 'a subsequent drag did not start');
      const pointerId = drag.pointerId;
      if (type === 'blur') window.dispatchEvent(new Event('blur'));
      else if (type === 'layout') renderKeyboard();
      else elements['keyboard-panel'].dispatchEvent(new PointerEvent(type, { pointerId, bubbles: true }));
      check(!drag && elements['selection-box'].hidden && !elements['keyboard-panel'].hasPointerCapture(pointerId), `${type} left a selection outline or pointer capture`);
      await pointer('mouseReleased', a);
    }

    let playbackAction = { type: 'wait', duration: 60000 };
    if (window.teaDesktopSmoke) {
      const cursor = await request('test-cursor');
      playbackAction = { type: 'move', x: cursor.targetX + (cursor.x === cursor.targetX ? 1 : 0), y: cursor.targetY, duration: 0 };
    }
    await commit({ version: 1, enabled: true, stopKey: 120,
      macros: [{ id: 1, name: 'playback test', actions: [playbackAction] }],
      bindings: [{ key: 'F1', code: 112, modifiers: 0, replace: false, macroId: 1 }] });
    if (window.teaDesktopSmoke) {
      const moved = await request('test-move', { code: 112 });
      check(moved.x === playbackAction.x && moved.y === playbackAction.y, 'native mouse movement did not reach its destination');
    }
    const playbackTest = nextState();
    playbackTest.macros[0].actions = [{ type: 'wait', duration: 60000 }];
    await commit(playbackTest);
    const gated = nextState(); gated.stopKey = 0;
    await commit(gated);
    check(elements.assign.disabled && elements['new-macro'].disabled, 'existing macros remain usable with no stop key');
    blocked = false;
    try { await request('test-trigger', { code: 112 }); } catch { blocked = true; }
    check(blocked, 'existing macros played without a configured stop key');
    const restored = nextState(); restored.stopKey = 120;
    await commit(restored);
    await request('test-trigger', { code: 112 });
    await until(() => playback.active);
    check(!elements['stop-playback'].hidden, 'stop playback is unavailable during playback');
    elements['stop-playback'].click();
    await until(() => !playback.active && elements['stop-playback'].hidden);
    await request('test-trigger', { code: 112 });
    await until(() => playback.active);
    await request('test-trigger', { code: 112 });
    showMacros();
    await request('test-trigger', { code: 120 });
    await until(() => !playback.active);
    elements.macros.close();
    check(!playback.paused, 'stop hotkey left playback paused');

    openEditor(macros[0]);
    const settingsBounds = elements['record-settings'].getBoundingClientRect();
    const imagesBounds = elements['open-images'].getBoundingClientRect();
    const conditionalsBounds = elements.conditionals.getBoundingClientRect();
    check(imagesBounds.left > settingsBounds.right && imagesBounds.top === settingsBounds.top, 'images is not beside recording settings');
    check(conditionalsBounds.left > imagesBounds.right && conditionalsBounds.top === imagesBounds.top &&
      elements.conditionals.textContent === 'conditionals', 'conditionals is not to the right of images');
    await request('test-preview', { page: 'image-target' });
    elements['open-images'].click();
    check(elements['images-dialog'].open && elements['images-empty'].hidden === (images.length > 0),
      'image library did not show its current contents in the editor');
    const image = (await request('test-import-image')).at(-1);
    applyImages(await request('image-library'));
    const imageRow = (id = image.id) => elements['image-list'].querySelector(`[data-image-id="${id}"]`);
    await until(() => imageRow()?.querySelector('img').naturalWidth > 0);
    const name = imageRow().querySelector('input');
    name.value = 'test target';
    name.dispatchEvent(new Event('input'));
    imageRow().querySelector('form button').click();
    await until(() => images.some(value => value.id === image.id && value.name === 'test target') && !imageBusy);
    await request('test-preview', { page: 'images' });
    const imageSettings = nextState();
    imageSettings.recordKey = 121;
    await commit(imageSettings);
    let capturedImage;
    if (window.teaDesktopSmoke) {
      elements['capture-image'].click();
      await until(() => !imageBusy);
      const screenCapture = (mouse, x, y) => request('test-image-capture', { mouse, x, y });
      const openedCapture = await screenCapture();
      check(openedCapture.toolbar && !openedCapture.selection && !openedCapture.app, 'opening capture showed a selection or left the app visible');
      const imageCount = images.length;
      await request('test-trigger', { code: 121 });
      check((await screenCapture()).toolbar && images.length === imageCount, 'record hotkey started a screen capture');
      await request('test-trigger', { code: 122 });
      await until(async () => (await screenCapture()).selection);
      check(!(await screenCapture()).toolbar, 'f11 left the capture toolbar visible');
      await screenCapture('down', 120, 100);
      const emptySelection = await screenCapture('up', 120, 100);
      check(emptySelection.selection && !emptySelection.toolbar && images.length === imageCount, 'a click saved an empty selection or restored the toolbar');
      await screenCapture('down', 120, 100);
      const dragging = await screenCapture('move', 40, 40);
      check(dragging.selection && !dragging.toolbar && images.length === imageCount, 'dragging saved an image or restored the toolbar before release');
      const completed = await screenCapture('up', 40, 40);
      check(completed.toolbar && !completed.selection && !completed.app, 'capture did not return to the toolbar');
      await until(() => images.length > imageCount);
      capturedImage = images.at(-1);
      check(capturedImage.name.startsWith('screen capture ') && capturedImage.width === 80 && capturedImage.height === 60,
        'screen capture did not save the dragged area');
      await request('test-trigger', { code: 122 });
      await until(async () => (await screenCapture()).selection);
      await screenCapture('down', 40, 40);
      await screenCapture('move', 120, 100);
      await request('test-trigger', { code: 120 });
      await until(async () => (await screenCapture()).app);
      const stopped = await screenCapture();
      check(!stopped.toolbar && !stopped.selection && images.length === imageCount + 1, 'stopping a drag left capture open or saved an image');

      await commit({ ...nextState(), stopKey: 122 });
      await updateImageLibrary('start-image-capture');
      await request('test-trigger', { code: 122 });
      await until(async () => (await screenCapture()).selection);
      await request('test-trigger', { code: 122 });
      await until(async () => (await screenCapture()).app);
      check(!(await screenCapture()).selection && images.length === imageCount + 1, 'f11 could not stop selection when assigned as the stop hotkey');
      await commit(imageSettings);
    } else {
      const library = await request('test-import-image');
      applyImages(library);
      capturedImage = library.at(-1);
    }
    check(getComputedStyle(elements['close-images']).borderTopWidth === '1px', 'the images close button is missing its border');
    elements['close-images'].click();
    elements['add-event'].click();
    document.querySelector('[data-add-event="wait-for-image"]').click();
    check(editorActions[selectedEvent]?.type === 'wait-for-image', 'wait for image could not be added');
    elements['save-macro'].click();
    check(elements.toast.textContent.includes('choose an image'), 'an image wait was accepted without a target');
    editEvent('imageId', image.id);
    check(elements.steps.querySelector('.event-image-caption')?.textContent === 'wait for test target', 'image block did not show its target');
    elements['save-macro'].click();
    await until(() => !flowStep && macros[0].actions.some(action => action.type === 'wait-for-image' && action.imageId === image.id));
    let usedImageProtected = false;
    try { await request('delete-image', { imageId: image.id }); }
    catch (error) { usedImageProtected = error.message.includes('used by the macro'); }
    check(usedImageProtected, 'a saved image wait could be left without its target');
    openEditor(macros[0]);
    selectEvent(editorActions.length - 1);
    check(Number(elements['event-fields'].querySelector('[name="imageId"]').value) === image.id, 'the saved image selection was lost');
    removeEvent();
    elements['add-event'].click();
    document.querySelector('[data-add-event="move"]').click();
    check(elements['event-fields'].querySelector('[name="destination"]').value === 'coordinates', 'coordinate moves are no longer the default');
    editEvent('x', -120);
    editEvent('destination', 'image');
    check(!elements['event-fields'].querySelector('[name="x"]') && elements['event-fields'].querySelector('[name="imageId"]'),
      'image destination did not replace the coordinate fields');
    elements['save-macro'].click();
    check(elements.toast.textContent.includes('choose an image'), 'a move to image was accepted without a target');
    editEvent('imageId', image.id);
    editEvent('destination', 'coordinates');
    check(elements['event-fields'].querySelector('[name="x"]').value === '-120', 'switching destinations lost the draft coordinates');
    editEvent('destination', 'image');
    check(Number(elements['event-fields'].querySelector('[name="imageId"]').value) === image.id &&
      elements.steps.querySelector('.event-image-caption')?.textContent === 'move to test target', 'image move lost its selected target');
    elements['save-macro'].click();
    await until(() => !flowStep && macros[0].actions.some(action => action.type === 'move' && action.destination === 'image' && action.imageId === image.id));
    usedImageProtected = false;
    try { await request('delete-image', { imageId: image.id }); }
    catch (error) { usedImageProtected = error.message.includes('used by the macro'); }
    check(usedImageProtected, 'a saved image move could be left without its target');
    openEditor(macros[0]);
    selectEvent(editorActions.length - 1);
    check(elements['event-fields'].querySelector('[name="destination"]').value === 'image' &&
      Number(elements['event-fields'].querySelector('[name="imageId"]').value) === image.id, 'the saved mouse destination was lost');
    removeEvent();
    elements['open-images'].click();
    imageRow().querySelector('.negative').click();
    check(elements['delete-confirmation'].open && elements['confirm-deletion'].textContent === 'save and delete',
      'image deletion did not offer to save the draft after its last reference was removed');
    await dismissDeletion();
    check(imageRow() && macros[0].actions.some(action => action.imageId === image.id), 'cancelling deletion changed the image or saved macro');
    imageRow().querySelector('.negative').click();
    elements['confirm-deletion'].click();
    await until(() => !imageRow());
    check(!(await request('image-library')).some(value => value.id === image.id), 'deleted image remains in the native library');
    check(macros[0].actions.length === 1 && macros[0].actions[0].type === 'wait', 'image deletion failed to save the remaining draft event');
    elements['close-images'].click();

    selectEvent(0);
    removeEvent();
    addAction({ ...defaultEvent('wait-for-image'), imageId: capturedImage.id });
    elements['save-macro'].click();
    await until(() => !flowStep);
    openEditor(macros[0]);
    selectEvent(0);
    removeEvent();
    elements['open-images'].click();
    imageRow(capturedImage.id).querySelector('.negative').click();
    check(elements['delete-confirmation'].open && elements['delete-question'].textContent.includes('key assignments') &&
      elements['confirm-deletion'].textContent === 'delete both', 'an empty draft did not offer to delete the saved macro and image together');
    await dismissDeletion();
    check(imageRow(capturedImage.id) && macros.length === 1 && state.bindings.length === 1, 'cancelling changed the image, macro, or binding');
    imageRow(capturedImage.id).querySelector('.negative').click();
    elements['confirm-deletion'].click();
    await until(() => !imageRow(capturedImage.id));
    check(!macros.length && !state.bindings.length && editingId === null, 'confirmed deletion left a saved macro or its binding behind');
    elements['close-images'].click();
    await closeEditor();

    const conditionImage = (await request('test-import-image')).at(-1);
    applyImages(await request('image-library'));
    openEditor();
    elements['macro-name'].value = 'conditional macro';
    elements['macro-name-form'].requestSubmit();
    document.querySelector('[data-macro-type="no-repeat"]').click();
    elements.conditionals.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true }));
    check(document.activeElement.dataset.addEvent === 'if-else', 'conditionals did not open with the keyboard');
    document.activeElement.click();
    const branch = editorActions[0];
    check(elements['conditionals-menu'].hidden && branch.type === 'if-else' &&
      elements['event-fields'].querySelector('[name="condition"]').value === 'image-on-screen', 'if / else did not show the image condition');
    elements['save-macro'].click();
    check(elements.toast.textContent.includes('choose an image'), 'if / else saved with no image selected');
    editEvent('imageId', conditionImage.id);
    const ifHeader = elements.steps.querySelector('[data-type="if-else"]');
    const [keyword, summary] = ifHeader.children;
    const ifBody = ifHeader.nextElementSibling;
    const [thenLane, elseLane] = ifBody.children;
    check(ifHeader.textContent === `if ${conditionImage.name} is on screen` &&
      keyword.getBoundingClientRect().right < summary.getBoundingClientRect().left &&
      ifHeader.offsetHeight < elements['add-event'].offsetHeight, 'if does not show a compact condition header');
    check(thenLane.firstElementChild.textContent === 'then' && elseLane.firstElementChild.textContent === 'else' &&
      thenLane.getBoundingClientRect().left === elseLane.getBoundingClientRect().left &&
      elseLane.getBoundingClientRect().top > thenLane.getBoundingClientRect().bottom &&
      parseFloat(getComputedStyle(ifBody).borderLeftWidth) > 0, 'if branches are missing their shared guide or aligned labels');
    function addToBranch(label, type) {
      const button = elements.steps.querySelector(`[aria-label="add event to ${label}"]`);
      button.click();
      button.nextElementSibling.querySelector(`[data-add-event="${type}"]`).click();
    }
    addToBranch('if then', 'wait');
    addToBranch('if else', 'while');
    elements['save-macro'].click();
    check(elements.toast.textContent.includes('choose an image') && selectedSequence === branch.elseActions,
      'save did not select the incomplete nested condition');
    editEvent('imageId', conditionImage.id);
    const loop = branch.elseActions[0];
    const whileHeader = elements.steps.querySelector('[data-type="while"]');
    const whileBody = whileHeader.nextElementSibling;
    const loopButton = whileBody.querySelector('.event-add');
    check(whileHeader.textContent === `while ${conditionImage.name} is on screen` &&
      !whileBody.querySelector('.event-branch-label') &&
      loopButton.offsetWidth < elements['add-event'].offsetWidth &&
      loopButton.getBoundingClientRect().left > whileBody.parentElement.getBoundingClientRect().left,
      'while does not show a compact header with an indented add button');
    for (const header of elements.steps.querySelectorAll('.event-condition')) {
      const [conditionType, description] = header.children;
      const imageName = description.firstElementChild;
      check(getComputedStyle(conditionType).color === 'rgb(250, 204, 21)' &&
        imageName?.textContent === conditionImage.name && getComputedStyle(imageName).color === 'rgb(34, 211, 238)' &&
        getComputedStyle(description).color === 'rgb(255, 255, 255)', 'condition colors do not distinguish the keyword and image name');
    }
    const nestedTransfer = new DataTransfer();
    elements.steps.querySelector('[data-type="if-else"]').dispatchEvent(new DragEvent('dragstart', { dataTransfer: nestedTransfer, bubbles: true }));
    const loopAdder = elements.steps.querySelector('[aria-label="add event to while do"]').parentElement;
    loopAdder.dispatchEvent(new DragEvent('drop', { dataTransfer: nestedTransfer, bubbles: true }));
    check(!loop.actions.length && editorActions[0] === branch, 'a block moved inside its own branch');
    elements.steps.querySelector('[data-type="wait"]').dispatchEvent(new DragEvent('dragstart', { dataTransfer: nestedTransfer, bubbles: true }));
    loopAdder.dispatchEvent(new DragEvent('drop', { dataTransfer: nestedTransfer, bubbles: true }));
    check(!branch.actions.length && loop.actions[0].type === 'wait', 'drag did not move an event into the loop body');
    const thenAdder = elements.steps.querySelector('[aria-label="add event to if then"]');
    thenAdder.click();
    thenAdder.nextElementSibling.querySelector('[data-record-inputs]').click();
    await until(() => recording && !recordingPending);
    await request('test-record', { mouse: true, key: 0, down: true, time: 100 });
    await request('test-record', { mouse: true, key: 0, down: false, time: 100 });
    await request('test-trigger', { code: state.stopKey });
    await until(() => !recording && !recordingPending);
    check(editorActions.length === 1 && branch.actions.map(action => action.mode).join() === 'down,up',
      'recording from a branch did not add inputs to that branch');
    elements.steps.querySelector('[data-type="if-else"]').click();
    check(inspectedAction === branch && Number(elements['event-fields'].querySelector('[name="imageId"]').value) === conditionImage.id,
      'clicking the if header did not open its condition settings');
    elements['save-macro'].click();
    await until(() => !flowStep);
    const savedBranch = macros[0].actions[0];
    check(savedBranch.actions[0].type === 'mouse' && savedBranch.elseActions[0].actions[0].type === 'wait', 'nested branches did not save');
    usedImageProtected = false;
    try { await request('delete-image', { imageId: conditionImage.id }); }
    catch (error) { usedImageProtected = error.message.includes('used by the macro'); }
    check(usedImageProtected, 'an image used by a condition could be deleted');
    openEditor(macros[0]);
    elements.steps.querySelector('[data-type="while"]').click();
    check(Number(elements['event-fields'].querySelector('[name="imageId"]').value) === conditionImage.id &&
      elements.steps.querySelector('[data-type="wait"]'), 'the loop condition or body was lost on reopening');
    elements.steps.querySelector('[data-type="wait"]').click();
    removeEvent();
    check(inspectedAction === editorActions[0].elseActions[0] && selectedEvent === -1 && !eventInspector.hidden &&
      elements['event-fields'].querySelector('[name="condition"]'), 'removing the last loop event did not show its parent fields');
    const completeLibrary = await request('test-import-image');
    const actionImage = completeLibrary.at(-1);
    applyImages(completeLibrary);
    addAction({ ...defaultEvent('wait-for-image'), imageId: actionImage.id });
    elements['save-macro'].click();
    await until(() => !flowStep);

    applyImages(completeLibrary.filter(entry => entry.id !== conditionImage.id && entry.id !== actionImage.id));
    openEditor(macros[0]);
    const recoveredLoop = editorActions[0].elseActions[0];
    const recoveredWait = recoveredLoop.actions[0];
    const missingId = Math.max(...completeLibrary.map(entry => entry.id)) + 1;
    recoveredLoop.condition.imageId = missingId;
    elements['save-macro'].click();
    check(inspectedAction === recoveredLoop && elements.toast.textContent.includes('choose an image'),
      'an edit introduced a missing image in a nested condition');
    recoveredLoop.condition.imageId = conditionImage.id;
    recoveredWait.imageId = missingId;
    elements['save-macro'].click();
    check(inspectedAction === recoveredWait && elements.toast.textContent.includes('choose an image'),
      'an edit introduced a missing image in a nested action');
    recoveredWait.imageId = actionImage.id;
    addAction({ ...defaultEvent('wait'), duration: 51 });
    elements['save-macro'].click();
    await until(() => !flowStep);
    const savedLoop = macros[0].actions[0].elseActions[0];
    check(savedLoop.condition.imageId === conditionImage.id && savedLoop.actions[0].imageId === actionImage.id &&
      savedLoop.actions[1].duration === 51, 'existing missing image references prevented a macro edit');
    applyImages(completeLibrary);

    await commit({ version: 1, enabled: false, stopKey: 120, recordKey: 121, layout: '75%',
      recordingOptions: { delays: false, clickPosition: true, keyboard: false, mouseButtons: true },
      bindingOptions: { modifiers: 5, replace: true },
      macros: [{ id: 999, name: 'persistence test', actions: [{ type: 'text', text: 'persisted', duration: 0 }] }],
      bindings: [{ key: 'F1', code: 112, modifiers: 3, replace: false, macroId: 999 }] });
    await request('delete-image', { imageId: conditionImage.id });
    await request('delete-image', { imageId: actionImage.id });
    native.postMessage({ command: 'test-result', ok: true });
  } catch (error) {
    native.postMessage({ command: 'test-result', ok: false, message: error.stack || error.message });
  }
})();
