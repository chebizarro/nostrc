# po/meson.build - Internationalization configuration for grotto
#
# This file configures gettext-based translation support.

i18n = import('i18n')

# Define the gettext domain
gettext_package = 'grotto'

# Generate and install translations
i18n.gettext(gettext_package,
  preset: 'glib',
)
