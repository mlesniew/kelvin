async function fetchReadings() {
  try {
    const res = await fetch('/readings');
    const data = await res.json();

    const tbody = document.getElementById('readingsTable');
    tbody.innerHTML = '';

    Object.entries(data).forEach(([mac, reading]) => {
      const row = document.createElement('tr');

      const name = reading.name || '—';
      const temp = reading.temperature?.toFixed(1) ?? '—';
      const hum = reading.humidity?.toFixed(1) ?? '—';
      const batt = reading.battery?.level ?? '—';
      const volt = reading.battery?.voltage?.toFixed(2) ?? '—';
      const age = reading.age?.toFixed(0) ?? '—';

      if (age !== null) {
        if (age <= 3) {
          row.classList.add('fresh');
        } else if (age > 180) {
          row.classList.add('stale');
        }
      }

      row.innerHTML = `
        <td>${name}</td>
        <td>${mac}</td>
        <td>${temp}</td>
        <td>${hum}</td>
        <td>${batt}</td>
        <td>${volt}</td>
        <td>${age}</td>
      `;

      tbody.appendChild(row);
    });

  } catch (err) {
    console.error("Error fetching /readings:", err);
  }
}

setInterval(fetchReadings, 3000);
fetchReadings();
