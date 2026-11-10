// melonDS Decomp Workbench documentation: placeholders for screenshots not taken yet, and the
// table of contents following the reader.
(function () {
  "use strict";

  // a frame whose screenshot is missing shows what it will show instead (data-pending)
  function watchFrame(frame) {
    var img = frame.querySelector("img");
    if (!img) return;
    function missing() { frame.classList.add("missing"); }
    if (img.complete && img.naturalWidth === 0) missing();
    else img.addEventListener("error", missing);
  }
  document.querySelectorAll(".frame").forEach(watchFrame);

  // the table of contents marks the section being read
  var links = Array.prototype.slice.call(document.querySelectorAll(".toc a[href^='#']"));
  if (!links.length || !("IntersectionObserver" in window)) return;
  var byId = {};
  links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
  var visible = {};
  var observer = new IntersectionObserver(function (entries) {
    entries.forEach(function (e) { visible[e.target.id] = e.isIntersecting; });
    var current = null;
    for (var i = 0; i < links.length; i++) {
      var id = links[i].getAttribute("href").slice(1);
      if (visible[id]) { current = id; break; }
    }
    if (!current) return;
    links.forEach(function (a) { a.classList.toggle("active", a === byId[current]); });
  }, { rootMargin: "-64px 0px -55% 0px" });
  Object.keys(byId).forEach(function (id) {
    var el = document.getElementById(id);
    if (el) observer.observe(el);
  });
})();
