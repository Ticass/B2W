const search = document.querySelector("#search");
const compile = document.querySelector("#compile");
const play = document.querySelector("#play");
const grid = document.querySelector("#grid");
const count = document.querySelector("#count");

function filterMaps() {
  const query = search.value.trim().toLocaleLowerCase();
  let shown = 0;
  for (const card of grid.querySelectorAll(".map-card")) {
    const matches = card.dataset.name.includes(query)
      && (compile.value === "all" || card.dataset.compile === compile.value)
      && (play.value === "all" || card.dataset.play === play.value);
    card.hidden = !matches;
    if (matches) {
      shown++;
    }
  }

  const empty = grid.querySelector(".empty");
  if (!shown && !empty) {
    const notice = document.createElement("div");
    notice.className = "empty";
    notice.textContent = "No maps match those filters.";
    grid.append(notice);
  } else if (shown && empty) {
    empty.remove();
  }
  count.textContent = `${shown.toLocaleString()} MAP${shown === 1 ? "" : "S"}`;
}

search.addEventListener("input", filterMaps);
compile.addEventListener("change", filterMaps);
play.addEventListener("change", filterMaps);
filterMaps();
