using SciMLTesting, ZamPlugins

# Docs env does not depend on this sublibrary yet, so rendering is unchecked.
run_qa(ZamPlugins; api_docs_kwargs = (; rendered = false))
