// Single source of truth for product/support contact details, shared by
// every page that needs to show it (dashboard, QR activation page,
// WhatsApp reminders, license banner). Replace the placeholder values
// below with the real ones — everything that displays them reads from
// here, so one edit updates all of them.
const PRODUCT_INFO = {
  name: 'Lights On',
  tagline: 'Smart room & slot booking system',
  phone: 'prefer email/whatsapp',        // shown as text / tel: link
  whatsapp: '919677020044',      // digits only, no "+", used in wa.me links
  email: 'lightson.srs@gmail.com' //, //  website: 'https://yourdomain.com'
};

// Plain-text one-liner — for anywhere only text is possible (WhatsApp
// message signature, plain banners, etc.)
function productInfoLine() {
  return `${PRODUCT_INFO.name} | for enquiry ${PRODUCT_INFO.email}`;
}

// Small HTML block — for anywhere markup can render (dashboard footer,
// QR activation page footer, About section).
function productInfoHtml() {
  return `<div class="product-info">
    <strong>${PRODUCT_INFO.name}</strong> &mdash; ${PRODUCT_INFO.tagline}<br>
    <a href="tel:${PRODUCT_INFO.phone}">${PRODUCT_INFO.phone}</a> &middot;
    <a href="https://wa.me/${PRODUCT_INFO.whatsapp}" target="_blank" rel="noopener">WhatsApp - ${PRODUCT_INFO.whatsapp}</a> &middot;
    <a href="mailto:${PRODUCT_INFO.email}">${PRODUCT_INFO.email}</a> &middot;
    </div>`;
}
