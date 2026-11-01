// The viewer of a PDF in a page: zebkit's small one in the place of Chrome's
// extension, over Chromium's own plugin, which draws, selects, finds, follows
// links and prints. This script is its scroller, its toolbar and its keys, as
// Chrome's viewport.ts and viewer-toolbar are. It runs in an isolated world in a
// closed shadow root of the PDF's document (pdf_viewer_renderer.cc).
(function (root, native) {
  'use strict';
  const doc = root.ownerDocument;
  const win = doc.defaultView;
  const scroller = root.getElementById('scroller');
  const embed = scroller.firstElementChild;
  const sizer = doc.createElement('div');
  sizer.id = 'sizer';
  scroller.appendChild(sizer);

  const kZooms = [0.25, 1 / 3, 0.5, 2 / 3, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4, 5];
  const kMinZoom = 0.25;
  const kMaxZoom = 5;
  const kArrowStep = 40;
  const hash = new URLSearchParams(win.location.hash.slice(1));

  let dimensions = null;
  let layoutOptions = null;
  let zoom = 1;
  let fit = 'width';        // 'width', 'page' or null for a set zoom.
  let formFocused = false;
  let sentLayout = false;
  let error = false;

  const sheet = new win.CSSStyleSheet();
  sheet.replaceSync(`
    #toolbar { position:absolute; left:0; right:0; top:0; height:40px; box-sizing:border-box; display:flex;
      align-items:center; gap:6px; padding:0 10px; color:#f1f3f4; background:#323639; font:13px system-ui,sans-serif;
      box-shadow:0 1px 3px rgba(0,0,0,.5); z-index:1; user-select:none; white-space:nowrap; overflow:hidden }
    #toolbar button { font:inherit; color:inherit; min-width:28px; height:28px; border:0; border-radius:50%; background:none; cursor:pointer }
    #toolbar button:hover { background:rgba(255,255,255,.12) }
    #toolbar input { user-select:text; font:inherit; color:inherit; border:0; border-radius:2px; background:#191b1c; height:22px; padding:0 4px }
    #page { width:42px; text-align:right } #find { width:150px } .gap { flex:1 } #message { position:absolute; inset:40px 0 0;
      display:none; align-items:center; justify-content:center; color:#f1f3f4; font:15px system-ui,sans-serif; background:rgb(40,40,40) }
    #sizer { width:1px }`);
  root.adoptedStyleSheets = [sheet];

  const toolbar = doc.createElement('div');
  toolbar.id = 'toolbar';
  const add = (tag, properties) => {
    const element = Object.assign(doc.createElement(tag), properties);
    toolbar.appendChild(element);
    return element;
  };
  const pageInput = add('input', {id: 'page', ariaLabel: 'Page number', value: '1'});
  const pageCount = add('span', {id: 'pages', textContent: '/ 0'});
  add('span', {className: 'gap'});
  const zoomOut = add('button', {id: 'zoom-out', title: 'Zoom out', textContent: '−'});
  const zoomText = add('span', {id: 'zoom', textContent: '100%'});
  const zoomIn = add('button', {id: 'zoom-in', title: 'Zoom in', textContent: '+'});
  const fitButton = add('button', {id: 'fit', title: 'Fit to page', textContent: '⤢'});
  add('span', {className: 'gap'});
  const findInput = add('input', {id: 'find', placeholder: 'Find in document', ariaLabel: 'Find in document'});
  const findText = add('span', {id: 'find-count'});
  const findUp = add('button', {id: 'find-previous', title: 'Previous', textContent: '↑'});
  const findDown = add('button', {id: 'find-next', title: 'Next', textContent: '↓'});
  const download = add('button', {id: 'download', title: 'Download', textContent: '⤓'});
  const print = add('button', {id: 'print', title: 'Print', textContent: '⎙'});
  const message = doc.createElement('div');
  message.id = 'message';
  root.append(toolbar, message);

  const showToolbar = hash.get('toolbar') !== '0';
  const layoutToolbar = () => {
    const shown = showToolbar && win.innerWidth >= 280 && win.innerHeight >= 140;
    toolbar.style.display = shown ? 'flex' : 'none';
    scroller.style.top = shown ? '40px' : '0';
  };
  layoutToolbar();

  const post = (object) => native.plugin.postMessage(object);

  const fitZoom = () => {
    if (!dimensions || !dimensions.width) return zoom;
    const width = scroller.clientWidth;
    const widthZoom = width / dimensions.width;
    if (fit === 'page') {
      const page = currentPageDimensions();
      const height = scroller.clientHeight;
      if (page) return clampZoom(Math.min(width / page.width, height / page.height));
      return clampZoom(widthZoom);
    }
    // As Chrome opens a document: its width, and no larger than itself.
    return clampZoom(Math.min(widthZoom, 1));
  };
  const clampZoom = (value) => Math.min(kMaxZoom, Math.max(kMinZoom, value));

  const send = () => {
    const viewport = {
      type: 'viewport', userInitiated: true, zoom,
      xOffset: scroller.scrollLeft, yOffset: scroller.scrollTop, pinchPhase: 0,
    };
    if (!sentLayout && layoutOptions) {
      viewport.layoutOptions = layoutOptions;
      sentLayout = true;
    }
    post(viewport);
  };

  const layoutSizer = () => {
    if (!dimensions) return;
    const height = dimensions.height * zoom;
    const width = dimensions.width * zoom;
    // The plugin's own height is in the flow, sticky at the scroller's top.
    sizer.style.height = Math.max(0, height - scroller.clientHeight) + 'px';
    sizer.style.width = Math.max(1, width) + 'px';
    embed.style.width = scroller.clientWidth + 'px';
    embed.style.height = scroller.clientHeight + 'px';
    zoomText.textContent = Math.round(zoom * 100) + '%';
  };

  const pages = () => (dimensions && dimensions.pageDimensions) || [];
  const currentPage = () => {
    const list = pages();
    if (!list.length) return 0;
    const top = scroller.scrollTop / zoom;
    const middle = top + scroller.clientHeight / zoom / 2;
    let low = 0;
    let high = list.length - 1;
    while (low < high) {
      const mid = (low + high + 1) >> 1;
      if (list[mid].y <= middle) low = mid; else high = mid - 1;
    }
    return low;
  };
  const currentPageDimensions = () => pages()[currentPage()];

  let stats = 0;
  const update = () => {
    const page = currentPage();
    if (root.activeElement !== pageInput) pageInput.value = String(page + 1);
    scroller.dataset.page = page + 1;
    scroller.dataset.pages = pages().length;
    scroller.dataset.zoom = zoom;
    if (!stats) {
      stats = win.setTimeout(() => {
        stats = 0;
        scroller.dataset.loadedPages = native.loaded();
        scroller.dataset.paintedPages = native.painted();
      }, 200);
    }
  };

  const setZoom = (value, keepFit) => {
    const next = clampZoom(value);
    const ratio = next / zoom;
    const left = scroller.scrollLeft;
    const top = scroller.scrollTop;
    if (!keepFit) fit = null;
    zoom = next;
    layoutSizer();
    scroller.scrollTo({left: left * ratio, top: top * ratio, behavior: 'instant'});
    send();
    update();
  };
  const stepZoom = (direction) => {
    const steps = direction > 0 ? kZooms : kZooms.slice().reverse();
    const next = steps.find((z) => (direction > 0 ? z > zoom + 1e-3 : z < zoom - 1e-3));
    if (next) setZoom(next);
  };
  const goTo = (index, y) => {
    const list = pages();
    if (!list.length) return;
    const page = list[Math.min(list.length - 1, Math.max(0, index))];
    scroller.scrollTo({top: ((page.y || 0) + (y || 0)) * zoom, behavior: 'instant'});
  };

  scroller.addEventListener('scroll', () => { send(); update(); }, {passive: true});
  new win.ResizeObserver(() => {
    layoutToolbar();
    if (fit) zoom = fitZoom();
    layoutSizer();
    send();
    update();
  }).observe(doc.documentElement);

  // Keys, as Chrome's viewport: the wheel and the trackpad are the scroller's own.
  const keyStep = {ArrowUp: [0, -kArrowStep], ArrowDown: [0, kArrowStep], ArrowLeft: [-kArrowStep, 0], ArrowRight: [kArrowStep, 0]};
  root.addEventListener('keydown', (event) => {
    const inField = event.target === pageInput || event.target === findInput;
    const control = event.ctrlKey || event.metaKey;
    if (control && !event.altKey) {
      const key = event.key.toLowerCase();
      if (key === 'f') { findInput.focus(); findInput.select(); event.preventDefault(); return; }
      if (key === 'g') { doFind(!event.shiftKey); event.preventDefault(); return; }
      if (key === 'a' && !inField && !formFocused) { post({type: 'selectAll'}); event.preventDefault(); return; }
      if (key === '=' || key === '+') { stepZoom(1); event.preventDefault(); return; }
      if (key === '-') { stepZoom(-1); event.preventDefault(); return; }
      if (key === '0') { fit = 'width'; setZoom(fitZoom(), true); event.preventDefault(); return; }
      if (key === 'p') { win.print(); event.preventDefault(); return; }
    }
    if (inField || formFocused || control || event.altKey) return;
    const height = scroller.clientHeight;
    let step = keyStep[event.key];
    if (event.key === 'PageDown' || (event.key === ' ' && !event.shiftKey)) step = [0, height * 0.9];
    if (event.key === 'PageUp' || (event.key === ' ' && event.shiftKey)) step = [0, -height * 0.9];
    if (step) {
      scroller.scrollBy({left: step[0], top: step[1], behavior: 'smooth'});
    } else if (event.key === 'Home') {
      scroller.scrollTo({top: 0, behavior: 'smooth'});
    } else if (event.key === 'End') {
      scroller.scrollTo({top: scroller.scrollHeight, behavior: 'smooth'});
    } else {
      return;
    }
    event.preventDefault();
  }, true);

  // Control and the wheel, and a trackpad's pinch, which comes as that: zoom.
  scroller.addEventListener('wheel', (event) => {
    if (!event.ctrlKey) return;
    event.preventDefault();
    setZoom(zoom * Math.exp(-event.deltaY / 200));
  }, {passive: false});

  pageInput.addEventListener('change', () => {
    const page = parseInt(pageInput.value, 10);
    if (page > 0) goTo(page - 1);
    embed.focus();
  });
  zoomIn.addEventListener('click', () => stepZoom(1));
  zoomOut.addEventListener('click', () => stepZoom(-1));
  fitButton.addEventListener('click', () => {
    fit = fit === 'page' ? 'width' : 'page';
    setZoom(fitZoom(), true);
    if (fit === 'page') goTo(currentPage());
  });

  // The fragment's #page=N, again when only it changes, as Chrome's viewer.
  win.addEventListener('hashchange', () => {
    const page = parseInt(new URLSearchParams(win.location.hash.slice(1)).get('page'), 10);
    if (page > 0) goTo(page - 1);
  });

  let lastFind = '';
  const doFind = (forward) => {
    const text = findInput.value;
    if (!text) {
      if (lastFind) native.findStop();
      lastFind = '';
      findText.textContent = '';
      return;
    }
    if (text !== lastFind) {
      lastFind = text;
      findOrdinal = 0;
      native.find(text, false);
    } else {
      native.findNext(forward);
    }
  };
  findInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { doFind(!event.shiftKey); event.preventDefault(); }
    if (event.key === 'Escape') {
      findInput.value = '';
      doFind(true);
      embed.focus();
      event.preventDefault();
    }
  });
  findUp.addEventListener('click', () => doFind(false));
  findDown.addEventListener('click', () => doFind(true));

  download.addEventListener('click', () => {
    const link = doc.createElement('a');
    link.href = win.location.href;
    link.download = '';
    link.click();
  });
  print.addEventListener('click', () => win.print());

  let findTotal = 0;
  let findOrdinal = 0;
  const showFind = () => { findText.textContent = findTotal ? `${findOrdinal}/${findTotal}` : '0/0'; scroller.dataset.findTotal = findTotal; scroller.dataset.findOrdinal = findOrdinal; };

  const allowedSchemes = new Set(['http:', 'https:', 'ftp:', 'mailto:', 'file:']);
  const navigate = (url, disposition) => {
    let target;
    try { target = new win.URL(url, win.location.href); } catch { return; }
    if (!allowedSchemes.has(target.protocol)) return;
    if (target.protocol === 'file:' && win.location.protocol !== 'file:') return;
    if (disposition === 0 && win.top === win) win.location.assign(target.href);
    else win.open(target.href, '_blank');
  };

  const handlers = {
    documentDimensions(data) {
      const first = !dimensions;
      dimensions = data;
      layoutOptions = data.layoutOptions;
      if (first) {
        const given = parseFloat(hash.get('zoom'));
        if (given > 0) { zoom = clampZoom(given / 100); fit = null; } else zoom = fitZoom();
      } else if (fit) {
        zoom = fitZoom();
      }
      pageCount.textContent = '/ ' + pages().length;
      layoutSizer();
      if (first && parseInt(hash.get('page'), 10) > 1) goTo(parseInt(hash.get('page'), 10) - 1);
      sentLayout = false;
      send();
      update();
    },
    loadProgress(data) {
      scroller.dataset.progress = data.progress;
      if (data.progress === -1 && !error) {
        error = true;
        message.textContent = "Couldn't load PDF document";
        message.style.display = 'flex';
      }
    },
    setScrollPosition(data) {
      const to = {behavior: 'instant'};
      if (data.x !== undefined) to.left = data.x;
      if (data.y !== undefined) to.top = data.y;
      scroller.scrollTo(to);
    },
    scrollBy(data) { scroller.scrollBy({left: data.x, top: data.y, behavior: 'instant'}); },
    goToPage(data) { goTo(data.page); },
    navigateToDestination(data) { goTo(data.page, data.y !== undefined ? pages()[data.page] && pages()[data.page].height - data.y : 0); },
    navigate(data) { navigate(data.url, data.disposition); },
    email(data) { navigate('mailto:' + data.to + '?cc=' + data.cc + '&bcc=' + data.bcc + '&subject=' + data.subject + '&body=' + data.body, 0); },
    formFocusChange(data) { formFocused = data.focused !== 'none'; },
    getPassword() {
      const given = win.prompt('This document is protected by a password. Enter it to open the document.', '');
      post({type: 'getPasswordComplete', password: given || ''});
    },
    getSelectedTextReply(data) { scroller.dataset.selectedText = data.selectedText; },
    zebkitFindCount(data) {
      findTotal = data.total;
      showFind();
      // Blink selects the first match after starting a find; so does this.
      if (data.final && findTotal && !findOrdinal) native.findNext(true);
    },
    zebkitFindSelection(data) { findOrdinal = data.ordinal; showFind(); },
  };
  return (data) => {
    const handler = handlers[data.type];
    if (handler) handler(data);
  };
})
