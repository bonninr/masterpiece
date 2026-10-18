// A link back to the project page, first among the top bar's right buttons.
(function () {
  var right = document.querySelector(".right-buttons");
  if (!right) return;
  var a = document.createElement("a");
  a.className = "mp-home";
  a.href = "/";
  a.title = "masterpieceorgan.com";
  a.textContent = "masterpieceorgan.com";
  right.insertBefore(a, right.firstChild);
})();
