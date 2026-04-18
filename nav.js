/* SoundRoot promo — shared nav JS */

// Theme toggle
(function () {
  const btn = document.getElementById('themeToggle');
  if (!btn) return;

  function update() {
    const dark = document.documentElement.classList.contains('dark');
    btn.textContent = dark ? 'Dark' : 'Light';
  }

  btn.addEventListener('click', function () {
    const dark = document.documentElement.classList.contains('dark');
    document.documentElement.classList.toggle('dark', !dark);
    document.documentElement.classList.toggle('light', dark);
    localStorage.setItem('sr-theme', dark ? 'light' : 'dark');
    update();
  });

  update();
})();
