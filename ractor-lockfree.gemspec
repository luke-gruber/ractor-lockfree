# frozen_string_literal: true

require_relative "lib/ractor/lockfree/version"

Gem::Specification.new do |spec|
  spec.name = "ractor-lockfree"
  spec.version = Ractor::Lockfree::VERSION
  spec.authors = ["Luke Gruber (Shopify)"]

  spec.summary = "Lock-free shareable data structures for Ractors"
  spec.description = "Data structures include Ractor::SharedVar"
  spec.license = "MIT"
  # spec.homepage = "https://github.com/<you>/ractor-lockfree"
  # spec.metadata["source_code_uri"] = spec.homepage
  spec.required_ruby_version = ">= 4.0"
  spec.extensions = %w[ext/ractor/shared_var/extconf.rb]

  spec.files = Dir["lib/**/*.rb", "ext/**/*.{c,h,rb}", "docs/*.md", "README.md", "LICENSE.txt"]
  spec.require_paths = ["lib"]
end
